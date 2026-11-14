using System;
using System.Diagnostics;
using System.IO;
using System.Numerics;
using System.Reflection;
using System.Runtime.InteropServices;
using Veldrid;

// Graphics probe for the osu! port: ppy.Veldrid (what osu-framework renders with) on the
// console's Vulkan driver, presenting to HBLauncher's vi window.  Each stage logs before it
// runs, so the last "stage N" line in the file-server log says where it stopped.
// hbxl_vk_shim.c provides the Vulkan loader and maps the Wayland swapchain source onto
// VK_NN_vi_surface.

[StructLayout(LayoutKind.Sequential)]
struct Vertex
{
    public Vector2 Position;
    public RgbaFloat Color;
    public Vertex(Vector2 p, RgbaFloat c) { Position = p; Color = c; }
}

static unsafe partial class Program
{
    [LibraryImport("libc", EntryPoint = "write")]
    private static partial nint write(int fd, byte* buf, nint count);

    [LibraryImport("hbxl", EntryPoint = "hbx_switch2_vi_window")]
    private static partial nint ViWindow();

    static void Log(string s)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes("gfx-probe: " + s + "\n");
        fixed (byte* p = bytes) write(2, p, bytes.Length);
    }

    static byte[] Resource(string name)
    {
        using var s = Assembly.GetExecutingAssembly().GetManifestResourceStream(name)
                      ?? throw new FileNotFoundException(name);
        var ms = new MemoryStream();
        s.CopyTo(ms);
        return ms.ToArray();
    }

    static int Main()
    {
        try
        {
            return Run();
        }
        catch (Exception e)
        {
            Log("FAILED: " + e);
            return 1;
        }
    }

    static int Run()
    {
        Log("stage 0: managed Main reached");

        nint window = ViWindow();
        Log($"stage 1: vi window 0x{window:x}");
        if (window == 0)
        {
            Log("FAILED: no vi window (manifest must say \"display\": \"exclusive\")");
            return 2;
        }

        Log("stage 2: GraphicsDevice.CreateVulkan (Wayland source -> VK_NN_vi_surface)");
        // Exactly osu-framework's VeldridDevice options: D16 swapchain depth, default clip/depth prefs.
        var options = new GraphicsDeviceOptions
        {
            HasMainSwapchain = true,
            SwapchainDepthFormat = PixelFormat.R16UNorm,
            SyncToVerticalBlank = true,
            ResourceBindingModel = ResourceBindingModel.Improved,
        };
        var source = SwapchainSource.CreateWayland(1, window);
        var desc = new SwapchainDescription(source, 1920, 1080, PixelFormat.R16UNorm, true);
        GraphicsDevice gd = GraphicsDevice.CreateVulkan(options, desc);
        Log($"stage 2: ok, {gd.DeviceName} / {gd.VendorName}, api {gd.ApiVersion}, backend {gd.BackendType}");
        var fb = gd.SwapchainFramebuffer;
        Log($"stage 2: swapchain {fb.Width}x{fb.Height}, color {fb.OutputDescription.ColorAttachments[0].Format}");

        ResourceFactory factory = gd.ResourceFactory;
        CommandList cl = factory.CreateCommandList();

        Log("stage 3: 120 frames of ClearColorTarget + SwapBuffers");
        var sw = Stopwatch.StartNew();
        for (int i = 0; i < 120; i++)
        {
            float t = i / 120f;
            cl.Begin();
            cl.SetFramebuffer(gd.SwapchainFramebuffer);
            cl.ClearColorTarget(0, new RgbaFloat(t, 0.2f, 1 - t, 1));
            cl.End();
            gd.SubmitCommands(cl);
            gd.SwapBuffers();
        }
        gd.WaitForIdle();
        Log($"stage 3: ok, {sw.Elapsed.TotalMilliseconds / 120:F2} ms/frame");

        Log("stage 4: buffers, SPIR-V shaders, graphics pipeline");
        DeviceBuffer vb = factory.CreateBuffer(new BufferDescription((uint)(3 * sizeof(Vertex)), BufferUsage.VertexBuffer));
        var layout = new VertexLayoutDescription(
            new VertexElementDescription("Position", VertexElementSemantic.TextureCoordinate, VertexElementFormat.Float2),
            new VertexElementDescription("Color", VertexElementSemantic.TextureCoordinate, VertexElementFormat.Float4));
        Shader vs = factory.CreateShader(new ShaderDescription(ShaderStages.Vertex, Resource("tri.vert.spv"), "main"));
        Shader fs = factory.CreateShader(new ShaderDescription(ShaderStages.Fragment, Resource("tri.frag.spv"), "main"));
        Pipeline pipeline = factory.CreateGraphicsPipeline(new GraphicsPipelineDescription(
            BlendStateDescription.SINGLE_OVERRIDE_BLEND,
            DepthStencilStateDescription.DISABLED,
            RasterizerStateDescription.CULL_NONE,
            PrimitiveTopology.TriangleList,
            new ShaderSetDescription(new[] { layout }, new[] { vs, fs }),
            Array.Empty<ResourceLayout>(),
            gd.SwapchainFramebuffer.OutputDescription));
        Log("stage 4: ok");

        Log("stage 5: 300 frames of a spinning triangle (vertex uploads + draws)");
        var verts = new Vertex[3];
        sw.Restart();
        for (int i = 0; i < 300; i++)
        {
            float a = i * 0.03f;
            for (int k = 0; k < 3; k++)
            {
                float ang = a + k * MathF.PI * 2 / 3;
                verts[k] = new Vertex(new Vector2(MathF.Cos(ang) * 0.6f, MathF.Sin(ang) * 0.6f * 16 / 9f),
                    k == 0 ? new RgbaFloat(1, 0, 0, 1) : k == 1 ? new RgbaFloat(0, 1, 0, 1) : new RgbaFloat(0, 0, 1, 1));
            }
            gd.UpdateBuffer(vb, 0, verts);
            cl.Begin();
            cl.SetFramebuffer(gd.SwapchainFramebuffer);
            cl.ClearColorTarget(0, new RgbaFloat(0.05f, 0.05f, 0.1f, 1));
            cl.SetPipeline(pipeline);
            cl.SetVertexBuffer(0, vb);
            cl.Draw(3);
            cl.End();
            gd.SubmitCommands(cl);
            gd.SwapBuffers();
            if (i % 100 == 99) Log($"stage 5: frame {i + 1}");
        }
        gd.WaitForIdle();
        Log($"stage 5: ok, {sw.Elapsed.TotalMilliseconds / 300:F2} ms/frame");

        Log("stage 7: GPU readback tests (texture upload, solid draw, textured draw, after filling memory)");
        try
        {
            ReadbackTests(gd);
            FrameworkLikeTests(gd);
        }
        catch (Exception e)
        {
            Log("stage 7: EXCEPTION " + e);
        }

        Log("stage 6: dispose everything (the launcher takes its display back)");
        pipeline.Dispose();
        vs.Dispose();
        fs.Dispose();
        vb.Dispose();
        cl.Dispose();
        gd.Dispose();
        Log("stage 6: ok");

        Log("RESULT: all stages passed");
        return 0;
    }

    const uint N = 64;

    static Texture Staging(ResourceFactory f, uint w, uint h) =>
        f.CreateTexture(TextureDescription.Texture2D(w, h, 1, 1, PixelFormat.R8G8B8A8UNorm, TextureUsage.Staging));

    /// <summary>Copies a texture to a staging texture and returns its pixels (tightly packed RGBA).</summary>
    static unsafe byte[] Read(GraphicsDevice gd, Texture src, uint w, uint h)
    {
        var f = gd.ResourceFactory;
        using Texture staging = Staging(f, w, h);
        using CommandList cl = f.CreateCommandList();
        cl.Begin();
        cl.CopyTexture(src, 0, 0, 0, 0, 0, staging, 0, 0, 0, 0, 0, w, h, 1, 1);
        cl.End();
        gd.SubmitCommands(cl);
        gd.WaitForIdle();
        MappedResource m = gd.Map(staging, MapMode.Read);
        var data = new byte[w * h * 4];
        try
        {
            for (uint y = 0; y < h; y++)
                new ReadOnlySpan<byte>((byte*)m.Data + y * m.RowPitch, (int)(w * 4)).CopyTo(data.AsSpan((int)(y * w * 4)));
        }
        finally
        {
            gd.Unmap(staging);
        }
        return data;
    }

    static byte[] Pattern(int seed)
    {
        var d = new byte[N * N * 4];
        for (int i = 0; i < d.Length; i++)
            d[i] = (byte)(i % 4 == 3 ? 255 : (i * 7 + seed * 31 + 3));
        return d;
    }

    static string Compare(byte[] want, byte[] got)
    {
        int bad = 0, zero = 0;
        for (int i = 0; i < want.Length; i++)
        {
            if (want[i] != got[i]) bad++;
            if (got[i] == 0) zero++;
        }
        if (bad == 0) return "OK";
        // A vertically flipped result is still a correct draw.
        int row = (int)N * 4, badFlip = 0;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < row; x++)
                if (want[y * row + x] != got[((int)N - 1 - y) * row + x]) badFlip++;
        if (badFlip == 0) return "OK (vertically flipped)";
        return $"WRONG: {bad}/{want.Length} bytes differ, {zero} bytes are zero, first pixel got {got[0]},{got[1]},{got[2]},{got[3]} want {want[0]},{want[1]},{want[2]},{want[3]}";
    }

    static void ReadbackTests(GraphicsDevice gd)
    {
        var f = gd.ResourceFactory;
        Shader vs = f.CreateShader(new ShaderDescription(ShaderStages.Vertex, Resource("fs.vert.spv"), "main"));
        Shader solidFs = f.CreateShader(new ShaderDescription(ShaderStages.Fragment, Resource("solid.frag.spv"), "main"));
        Shader texFs = f.CreateShader(new ShaderDescription(ShaderStages.Fragment, Resource("tex.frag.spv"), "main"));
        ResourceLayout layout = f.CreateResourceLayout(new ResourceLayoutDescription(
            new ResourceLayoutElementDescription("Tex", ResourceKind.TextureReadOnly, ShaderStages.Fragment),
            new ResourceLayoutElementDescription("Samp", ResourceKind.Sampler, ShaderStages.Fragment)));
        Texture rt = f.CreateTexture(TextureDescription.Texture2D(N, N, 1, 1, PixelFormat.R8G8B8A8UNorm, TextureUsage.RenderTarget | TextureUsage.Sampled));
        Framebuffer fb = f.CreateFramebuffer(new FramebufferDescription(null, rt));
        Pipeline Make(Shader frag, ResourceLayout[] layouts) => f.CreateGraphicsPipeline(new GraphicsPipelineDescription(
            BlendStateDescription.SINGLE_OVERRIDE_BLEND, DepthStencilStateDescription.DISABLED, RasterizerStateDescription.CULL_NONE,
            PrimitiveTopology.TriangleList, new ShaderSetDescription(Array.Empty<VertexLayoutDescription>(), new[] { vs, frag }),
            layouts, fb.OutputDescription));
        Pipeline solid = Make(solidFs, Array.Empty<ResourceLayout>());
        Pipeline textured = Make(texFs, new[] { layout });
        CommandList cl = f.CreateCommandList();

        void Draw(Pipeline pl, ResourceSet? set)
        {
            cl.Begin();
            cl.SetFramebuffer(fb);
            cl.ClearColorTarget(0, new RgbaFloat(1, 0, 1, 1));
            cl.SetPipeline(pl);
            if (set != null) cl.SetGraphicsResourceSet(0, set);
            cl.Draw(3);
            cl.End();
            gd.SubmitCommands(cl);
            gd.WaitForIdle();
        }

        string UploadTest(string what, Texture tex, int seed)
        {
            byte[] want = Pattern(seed);
            gd.UpdateTexture(tex, want, 0, 0, 0, N, N, 1, 0, 0);
            return Compare(want, Read(gd, tex, N, N));
        }

        string TexturedDrawTest(Texture tex, int seed)
        {
            using ResourceSet set = f.CreateResourceSet(new ResourceSetDescription(layout, tex, gd.PointSampler));
            Draw(textured, set);
            return Compare(Pattern(seed), Read(gd, rt, N, N));
        }

        var tex1 = f.CreateTexture(TextureDescription.Texture2D(N, N, 1, 1, PixelFormat.R8G8B8A8UNorm, TextureUsage.Sampled));
        Log("stage 7a: texture upload + readback: " + UploadTest("tex1", tex1, 1));

        Draw(solid, null);
        byte[] px = Read(gd, rt, N, N);
        Log($"stage 7b: solid draw into offscreen target: pixel {px[0]},{px[1]},{px[2]},{px[3]} (want 64,128,191,255; magenta 255,0,255 = clear only, draw lost)");

        Log("stage 7c: textured draw (sample the uploaded texture): " + TexturedDrawTest(tex1, 1));

        // Push allocations into later device-memory blocks (WiiCompiled saw copies into
        // textures come out empty there with Dawn), then test fresh textures again.
        var ballast = new System.Collections.Generic.List<Texture>();
        var big = new byte[1024 * 1024 * 4];
        for (int i = 0; i < 48; i++)
        {
            var t = f.CreateTexture(TextureDescription.Texture2D(1024, 1024, 1, 1, PixelFormat.R8G8B8A8UNorm, TextureUsage.Sampled));
            gd.UpdateTexture(t, big, 0, 0, 0, 1024, 1024, 1, 0, 0);
            ballast.Add(t);
        }
        Log("stage 7d: allocated 48 x 4 MiB ballast textures (192 MiB)");
        var tex2 = f.CreateTexture(TextureDescription.Texture2D(N, N, 1, 1, PixelFormat.R8G8B8A8UNorm, TextureUsage.Sampled));
        Log("stage 7e: upload + readback after ballast: " + UploadTest("tex2", tex2, 2));
        Log("stage 7f: textured draw after ballast: " + TexturedDrawTest(tex2, 2));
        Log("stage 7g: upload into the LAST ballast texture: " + UploadTest("last ballast", ballast[^1], 3));

        foreach (var t in ballast) t.Dispose();
        tex1.Dispose();
        tex2.Dispose();
        cl.Dispose();
        solid.Dispose();
        textured.Dispose();
        fb.Dispose();
        rt.Dispose();
        layout.Dispose();
        vs.Dispose();
        solidFs.Dispose();
        texFs.Dispose();
        Log("stage 7: done");
    }

    [StructLayout(LayoutKind.Sequential)]
    struct QuadVertex
    {
        public Vector2 Position;
        public float Depth;
    }

    /// <summary>Stage 8: osu-framework's draw setup, one feature at a time, checked by readback.</summary>
    static unsafe void FrameworkLikeTests(GraphicsDevice gd)
    {
        Log($"stage 8: framework-like draws (clip Y inverted {gd.IsClipSpaceYInverted}, depth 0..1 {gd.IsDepthRangeZeroToOne}, UBO align {gd.UniformBufferMinOffsetAlignment})");
        var f = gd.ResourceFactory;
        Texture colour = f.CreateTexture(TextureDescription.Texture2D(N, N, 1, 1, PixelFormat.R8G8B8A8UNorm, TextureUsage.RenderTarget | TextureUsage.Sampled));
        Texture depth = f.CreateTexture(TextureDescription.Texture2D(N, N, 1, 1, PixelFormat.R16UNorm, TextureUsage.DepthStencil));
        Framebuffer fb = f.CreateFramebuffer(new FramebufferDescription(depth, colour));

        var quad = new[]
        {
            new QuadVertex { Position = new Vector2(-1, -1), Depth = 0.5f },
            new QuadVertex { Position = new Vector2(1, -1), Depth = 0.5f },
            new QuadVertex { Position = new Vector2(1, 1), Depth = 0.5f },
            new QuadVertex { Position = new Vector2(-1, 1), Depth = 0.5f },
        };
        DeviceBuffer vb = f.CreateBuffer(new BufferDescription((uint)(4 * sizeof(QuadVertex)), BufferUsage.VertexBuffer));
        gd.UpdateBuffer(vb, 0, quad);
        DeviceBuffer ib = f.CreateBuffer(new BufferDescription(6 * sizeof(ushort), BufferUsage.IndexBuffer));
        gd.UpdateBuffer(ib, 0, new ushort[] { 0, 1, 2, 2, 3, 0 });
        DeviceBuffer ubo = f.CreateBuffer(new BufferDescription(64, BufferUsage.UniformBuffer));

        var layout = new VertexLayoutDescription(
            new VertexElementDescription("Position", VertexElementSemantic.TextureCoordinate, VertexElementFormat.Float2),
            new VertexElementDescription("Depth", VertexElementSemantic.TextureCoordinate, VertexElementFormat.Float1));
        Shader vs = f.CreateShader(new ShaderDescription(ShaderStages.Vertex, Resource("idx.vert.spv"), "main"));
        Shader solidFs = f.CreateShader(new ShaderDescription(ShaderStages.Fragment, Resource("solid.frag.spv"), "main"));
        Shader uboFs = f.CreateShader(new ShaderDescription(ShaderStages.Fragment, Resource("ubo.frag.spv"), "main"));
        Shader uboTexFs = f.CreateShader(new ShaderDescription(ShaderStages.Fragment, Resource("ubotex.frag.spv"), "main"));

        ResourceLayout uboLayout = f.CreateResourceLayout(new ResourceLayoutDescription(
            new ResourceLayoutElementDescription("Globals", ResourceKind.UniformBuffer, ShaderStages.Vertex | ShaderStages.Fragment)));
        ResourceLayout texLayout = f.CreateResourceLayout(new ResourceLayoutDescription(
            new ResourceLayoutElementDescription("Tex", ResourceKind.TextureReadOnly, ShaderStages.Fragment),
            new ResourceLayoutElementDescription("Samp", ResourceKind.Sampler, ShaderStages.Fragment)));
        ResourceSet uboSet = f.CreateResourceSet(new ResourceSetDescription(uboLayout, ubo));
        Texture white = f.CreateTexture(TextureDescription.Texture2D(1, 1, 1, 1, PixelFormat.R8G8B8A8UNorm, TextureUsage.Sampled));
        gd.UpdateTexture(white, new byte[] { 255, 255, 255, 255 }, 0, 0, 0, 1, 1, 1, 0, 0);
        ResourceSet texSet = f.CreateResourceSet(new ResourceSetDescription(texLayout, white, gd.LinearSampler));

        var depthWrite = new DepthStencilStateDescription(true, true, ComparisonKind.LessEqual);
        var depthTestOnly = new DepthStencilStateDescription(true, false, ComparisonKind.LessEqual);
        var alphaBlend = BlendStateDescription.SINGLE_ALPHA_BLEND;
        var opaque = BlendStateDescription.SINGLE_OVERRIDE_BLEND;

        Pipeline Make(Shader frag, ResourceLayout[] layouts, DepthStencilStateDescription ds, BlendStateDescription blend, bool scissor)
        {
            var raster = RasterizerStateDescription.CULL_NONE;
            raster.ScissorTestEnabled = scissor;
            return f.CreateGraphicsPipeline(new GraphicsPipelineDescription(blend, ds, raster, PrimitiveTopology.TriangleList,
                new ShaderSetDescription(new[] { layout }, new[] { vs, frag }), layouts, fb.OutputDescription));
        }

        CommandList cl = f.CreateCommandList();

        // One test: clear (magenta, depth 1), optionally a first draw, then the tested draw; read pixel (8, 8).
        string Run(string what, Pipeline pl, ResourceSet?[] sets, RgbaFloat? tint, Action<CommandList>? before, string want)
        {
            if (tint is RgbaFloat t)
                gd.UpdateBuffer(ubo, 0, new[] { t });
            cl.Begin();
            cl.SetFramebuffer(fb);
            cl.ClearColorTarget(0, new RgbaFloat(1, 0, 1, 1));
            cl.ClearDepthStencil(1f);
            before?.Invoke(cl);
            cl.SetPipeline(pl);
            for (uint i = 0; i < sets.Length; i++)
                if (sets[i] != null) cl.SetGraphicsResourceSet(i, sets[i]);
            cl.SetVertexBuffer(0, vb);
            cl.SetIndexBuffer(ib, IndexFormat.UInt16);
            cl.DrawIndexed(6, 1, 0, 0, 0);
            cl.End();
            gd.SubmitCommands(cl);
            gd.WaitForIdle();
            byte[] px = Read(gd, colour, N, N);
            int o = (8 * (int)N + 8) * 4;
            string got = $"{px[o]},{px[o + 1]},{px[o + 2]},{px[o + 3]}";
            string verdict = got == "255,0,255,255" ? "FAIL (only the clear: draw discarded)" : "ok";
            return $"{what}: pixel {got} (want ~{want}) {verdict}";
        }

        Pipeline pSolid = Make(solidFs, Array.Empty<ResourceLayout>(), DepthStencilStateDescription.DISABLED, opaque, false);
        Log("stage 8a " + Run("indexed quad, no depth test", pSolid, Array.Empty<ResourceSet?>(), null, null, "64,128,191"));

        Pipeline pDepthWrite = Make(solidFs, Array.Empty<ResourceLayout>(), depthWrite, opaque, false);
        Log("stage 8b " + Run("depth test LessEqual + write (front pass)", pDepthWrite, Array.Empty<ResourceSet?>(), null, null, "64,128,191"));

        Pipeline pUboTestOnly = Make(uboFs, new[] { uboLayout }, depthTestOnly, opaque, false);
        Log("stage 8c " + Run("depth-written first, then depth test only (back pass)", pUboTestOnly, new ResourceSet?[] { uboSet },
            new RgbaFloat(0, 1, 0, 1), c =>
            {
                c.SetPipeline(pDepthWrite);
                c.SetVertexBuffer(0, vb);
                c.SetIndexBuffer(ib, IndexFormat.UInt16);
                c.DrawIndexed(6, 1, 0, 0, 0);
            }, "0,255,0"));

        Pipeline pUbo = Make(uboFs, new[] { uboLayout }, DepthStencilStateDescription.DISABLED, opaque, false);
        Log("stage 8d " + Run("colour from uniform buffer (set 0)", pUbo, new ResourceSet?[] { uboSet }, new RgbaFloat(1, 0.5f, 0, 1), null, "255,128,0"));

        Pipeline pUboTex = Make(uboTexFs, new[] { uboLayout, texLayout }, DepthStencilStateDescription.DISABLED, opaque, false);
        Log("stage 8e " + Run("UBO set 0 + texture/sampler set 1 (framework layout)", pUboTex, new ResourceSet?[] { uboSet, texSet }, new RgbaFloat(0, 0.5f, 1, 1), null, "0,128,255"));

        Pipeline pScissor = Make(uboTexFs, new[] { uboLayout, texLayout }, DepthStencilStateDescription.DISABLED, opaque, true);
        Log("stage 8f " + Run("+ scissor (full rect)", pScissor, new ResourceSet?[] { uboSet, texSet }, new RgbaFloat(0, 0.5f, 1, 1),
            c => c.SetScissorRect(0, 0, 0, N, N), "0,128,255"));

        Pipeline pAll = Make(uboTexFs, new[] { uboLayout, texLayout }, depthTestOnly, alphaBlend, true);
        Log("stage 8g " + Run("everything: depth test, scissor, alpha blend", pAll, new ResourceSet?[] { uboSet, texSet }, new RgbaFloat(1, 1, 0, 1),
            c => c.SetScissorRect(0, 0, 0, N, N), "255,255,0"));

        foreach (var d in new IDisposable[] { pSolid, pDepthWrite, pUboTestOnly, pUbo, pUboTex, pScissor, pAll, cl, uboSet, texSet, white,
                     uboLayout, texLayout, vs, solidFs, uboFs, uboTexFs, ubo, ib, vb, fb, depth, colour })
            d.Dispose();
        Log("stage 8: done");
    }
}
