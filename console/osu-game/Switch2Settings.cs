// osu! on Switch 2 (kaiHEN): the "Switch 2" section of the in-game settings.
//
// Display, gyro aiming, Joy-Con mouse and USB keyboard/mouse options live only here (LX_NO_KBMOUSE=1 in lx-env is the
// one escape hatch). The audio output queue and the diagnostics are here too but keep their lx-env lines, because a bad
// value can break things before the settings can be reached: the lx-env value is the default of the setting, and a
// value changed in the game is saved and wins from then on (the revert button goes back to the lx-env value).
// Everything is stored in switch2.ini next to osu!'s own config, changed values only. Startup-only options stay in
// lx-env (/switch/osu/lx-env on the SD card, then the .nro's built-in defaults).
//
// The values reach the C shims through small setters (shim/lx_input.c, lx_audio_out.c, lx_gc.c, lx_osu.c). On the
// desktop comparison build the setters do nothing, so the section can be looked at there.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using osu.Framework.Bindables;
using osu.Framework.Configuration;
using osu.Framework.Graphics;
using osu.Framework.Graphics.Sprites;
using osu.Framework.Localisation;
using osu.Framework.Platform;
using osu.Game.Graphics.UserInterfaceV2;
using osu.Game.Overlays.Settings;

public enum Switch2Setting
{
    GyroButton,
    GyroSpeedX,
    GyroSpeedY,
    GyroInvertX,
    GyroInvertY,
    UsbKeyboardMouse,
    MouseSpeed,
    JoyConMouse,
    JoyConMouseSpeed,
    AudioPeriod,
    AudioBuffers,
    LogGarbageCollections,
    ImportAutoCheck,
    ImportCheckInterval,
    HandheldResolution,
    DockedResolution,
    FrameRate,
}

public partial class Switch2Config : IniConfigManager<Switch2Setting>
{
    /// <summary>Gyro buttons in the order of hbxl_input_set_gyro's index (shim/lx_input.c).</summary>
    public static readonly string[] GYRO_BUTTONS = { "Off", "B", "L", "R", "ZL", "ZR", "L3", "R3" };

    /// <summary>Joy-Con mouse choices; the index is hbxl_input_set_joymouse's side mask (bit 0 left, bit 1 right).</summary>
    public static readonly string[] JOYCON_MOUSE = { "Off", "Left Joy-Con only", "Right Joy-Con only", "Both Joy-Cons" };

    /// <summary>Display choices and the window height each one asks the host for (0: the console's default).</summary>
    public static readonly string[] HANDHELD_RESOLUTION = { "1080p (native)", "900p", "720p" };

    private static readonly int[] handheld_heights = { 1080, 900, 720 };

    public static readonly string[] DOCKED_RESOLUTION = { "Automatic (TV output)", "2160p (4K)", "1440p", "1080p", "720p" };

    private static readonly int[] docked_heights = { 0, 2160, 1440, 1080, 720 };

    /// <summary>Frame rate choices and the cap each one asks the host for (0: every refresh at the highest rate).</summary>
    public static readonly string[] FRAME_RATE = { "Highest (120 Hz where available)", "60 fps" };

    private static readonly int[] frame_rates = { 0, 60 };

    private const string filename = "switch2.ini";

    protected override string Filename => filename;

    // The file only ever holds this enum's keys; unknown lines are dropped instead of becoming string settings.
    protected override bool AddMissingEntries => false;

    private readonly Storage storage;

    /// <summary>HBXL_GC_LOG's threshold (ms) for the log line per collection; the setting only turns the line on and off.</summary>
    private int gcLogMinPause;

    public Switch2Config(Storage storage)
        : base(storage)
    {
        this.storage = storage;
    }

    protected override void InitialiseDefaults()
    {
        SetDefault(Switch2Setting.GyroButton, "B");
        SetDefault(Switch2Setting.GyroSpeedX, 2.5f, 0.1f, 10f, 0.1f);
        SetDefault(Switch2Setting.GyroSpeedY, 1.5f, 0.1f, 10f, 0.1f);
        SetDefault(Switch2Setting.GyroInvertX, false);
        SetDefault(Switch2Setting.GyroInvertY, false);

        // LX_NO_KBMOUSE=1 in lx-env still forces both off in the shim (escape hatch), whatever this says.
        SetDefault(Switch2Setting.UsbKeyboardMouse, true);
        SetDefault(Switch2Setting.MouseSpeed, 1f, 0.1f, 10f, 0.1f);

        SetDefault(Switch2Setting.JoyConMouse, "Both Joy-Cons");
        SetDefault(Switch2Setting.JoyConMouseSpeed, 1f, 0.1f, 10f, 0.1f);

        // The import folder on the SD card (ConsoleOsuGame.importFromFolder): checked every N seconds for new files.
        SetDefault(Switch2Setting.ImportAutoCheck, true);
        SetDefault(Switch2Setting.ImportCheckInterval, 15, 5, 300);

        SetDefault(Switch2Setting.HandheldResolution, HANDHELD_RESOLUTION[0]);
        SetDefault(Switch2Setting.DockedResolution, DOCKED_RESOLUTION[0]);
        SetDefault(Switch2Setting.FrameRate, FRAME_RATE[0]);

        // The audio queue and the diagnostics keep their lx-env lines, which are the defaults here.
        SetDefault(Switch2Setting.AudioPeriod, envInt("HBXL_AUDIO_PERIOD", 128, 16, 1024), 16, 1024);
        SetDefault(Switch2Setting.AudioBuffers, envInt("HBXL_AUDIO_BUFFERS", 2, 1, 16), 1, 16);

        // HBXL_GC_LOG: unset = every collection, 0 = off, N = only pauses of N ms and longer.
        string? gcLog = env("HBXL_GC_LOG");
        gcLogMinPause = gcLog != null && int.TryParse(gcLog, NumberStyles.Integer, CultureInfo.InvariantCulture, out int ms) && ms > 0 ? ms : 0;
        SetDefault(Switch2Setting.LogGarbageCollections, gcLog != "0");
    }

    /// <summary>
    /// Only settings that differ from their default are written, so an untouched setting keeps following lx-env.
    /// </summary>
    protected override bool PerformSave()
    {
        try
        {
            using (var stream = storage.CreateFileSafely(Filename))
            using (var w = new StreamWriter(stream))
            {
                foreach (var p in ConfigStore)
                {
                    if (p.Value is IHasDefaultValue d && d.IsDefault)
                        continue;

                    w.WriteLine("{0} = {1}", p.Key, p.Value.ToString(null, CultureInfo.InvariantCulture)?.Replace("\n", "").Replace("\r", ""));
                }
            }
        }
        catch
        {
            return false;
        }

        return true;
    }

    /// <summary>The queue length the audio shim reported after the last change, in ms (0: its output isn't open).</summary>
    public readonly BindableDouble AudioQueueMs = new BindableDouble();

    /// <summary>
    /// Sends every setting to the shims, now and on each change. Called once, before the game loads: the audio queue is
    /// then in place before the output device opens.
    /// </summary>
    public void Apply()
    {
        var gyroButton = GetBindable<string>(Switch2Setting.GyroButton);
        var gyroSpeedX = GetBindable<float>(Switch2Setting.GyroSpeedX);
        var gyroSpeedY = GetBindable<float>(Switch2Setting.GyroSpeedY);
        var gyroInvertX = GetBindable<bool>(Switch2Setting.GyroInvertX);
        var gyroInvertY = GetBindable<bool>(Switch2Setting.GyroInvertY);

        void gyro() => Switch2Native.SetGyro(Math.Max(0, Array.IndexOf(GYRO_BUTTONS, gyroButton.Value)), gyroSpeedX.Value, gyroSpeedY.Value,
            gyroInvertX.Value, gyroInvertY.Value);

        gyroButton.ValueChanged += _ => gyro();
        gyroSpeedX.ValueChanged += _ => gyro();
        gyroSpeedY.ValueChanged += _ => gyro();
        gyroInvertX.ValueChanged += _ => gyro();
        gyroInvertY.ValueChanged += _ => gyro();
        gyro();

        var usb = GetBindable<bool>(Switch2Setting.UsbKeyboardMouse);
        var mouseSpeed = GetBindable<float>(Switch2Setting.MouseSpeed);

        void kbMouse() => Switch2Native.SetKeyboardMouse(usb.Value, mouseSpeed.Value);

        usb.ValueChanged += _ => kbMouse();
        mouseSpeed.ValueChanged += _ => kbMouse();
        kbMouse();

        var joyMouse = GetBindable<string>(Switch2Setting.JoyConMouse);
        var joyMouseSpeed = GetBindable<float>(Switch2Setting.JoyConMouseSpeed);

        void joyConMouse() => Switch2Native.SetJoyConMouse(Math.Max(0, Array.IndexOf(JOYCON_MOUSE, joyMouse.Value)), joyMouseSpeed.Value);

        joyMouse.ValueChanged += _ => joyConMouse();
        joyMouseSpeed.ValueChanged += _ => joyConMouse();
        joyConMouse();

        var period = GetBindable<int>(Switch2Setting.AudioPeriod);
        var buffers = GetBindable<int>(Switch2Setting.AudioBuffers);

        void audio() => AudioQueueMs.Value = Switch2Native.SetAudioQueue(period.Value, buffers.Value) / 1000.0;

        period.ValueChanged += _ => audio();
        buffers.ValueChanged += _ => audio();
        audio();

        GetBindable<bool>(Switch2Setting.LogGarbageCollections).BindValueChanged(e => Switch2Native.SetGcLog(e.NewValue ? gcLogMinPause : -1), true);

        // The display was set before the window was made (ApplyDisplayEarly); only changes are sent from here on.
        var handheld = GetBindable<string>(Switch2Setting.HandheldResolution);
        var docked = GetBindable<string>(Switch2Setting.DockedResolution);
        var frameRate = GetBindable<string>(Switch2Setting.FrameRate);

        void display() => setDisplay(handheld.Value, docked.Value, frameRate.Value);

        handheld.ValueChanged += _ => display();
        docked.ValueChanged += _ => display();
        frameRate.ValueChanged += _ => display();

        // Keeps the bindables (and their handlers) alive as long as the config.
        applied = new IBindable[] { gyroButton, gyroSpeedX, gyroSpeedY, gyroInvertX, gyroInvertY, usb, mouseSpeed, joyMouse, joyMouseSpeed, period, buffers, handheld, docked, frameRate };
    }

    private IBindable[]? applied;

    private static int pick(string[] names, int[] values, string? name) => values[Math.Max(0, Array.IndexOf(names, name))];

    private static void setDisplay(string? handheld, string? docked, string? frameRate) =>
        Switch2Native.SetDisplay(pick(HANDHELD_RESOLUTION, handheld_heights, handheld), pick(DOCKED_RESOLUTION, docked_heights, docked),
            pick(FRAME_RATE, frame_rates, frameRate));

    /// <summary>
    /// Sends the display settings to the host before the game host makes its window, so the first swapchain already has
    /// the right size. Reads switch2.ini directly (the config manager needs the game's storage, which comes later); a
    /// missing file or line means the default.
    /// </summary>
    public static void ApplyDisplayEarly()
    {
        string? handheld = null, docked = null, frameRate = null;

        try
        {
            string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "osu", filename);

            if (File.Exists(path))
            {
                foreach (string line in File.ReadAllLines(path))
                {
                    int eq = line.IndexOf('=');
                    if (eq < 0) continue;

                    string key = line[..eq].Trim(), value = line[(eq + 1)..].Trim();

                    if (key == nameof(Switch2Setting.HandheldResolution)) handheld = value;
                    else if (key == nameof(Switch2Setting.DockedResolution)) docked = value;
                    else if (key == nameof(Switch2Setting.FrameRate)) frameRate = value;
                }
            }
        }
        catch (Exception e)
        {
            Program.Log($"switch2 settings: display settings unreadable, using the defaults: {e.Message}");
        }

        setDisplay(handheld, docked, frameRate);
    }

    private static string? env(string name)
    {
        string? value = Environment.GetEnvironmentVariable(name)?.Trim();
        return string.IsNullOrEmpty(value) ? null : value;
    }

    private static int envInt(string name, int fallback, int min, int max) =>
        int.TryParse(env(name), NumberStyles.Integer, CultureInfo.InvariantCulture, out int v) ? Math.Clamp(v, min, max) : fallback;
}

internal static partial class Switch2Native
{
#if HBXL
    [LibraryImport("hbxl", EntryPoint = "hbxl_input_set_gyro")]
    private static partial void hbxl_input_set_gyro(int button, float speedX, float speedY, int invertX, int invertY);

    [LibraryImport("hbxl", EntryPoint = "hbxl_input_set_kbmouse")]
    private static partial void hbxl_input_set_kbmouse(int enabled, float speed);

    [LibraryImport("hbxl", EntryPoint = "hbxl_input_set_joymouse")]
    private static partial void hbxl_input_set_joymouse(int sides, float speed);

    [LibraryImport("hbxl", EntryPoint = "hbxl_audio_set_queue")]
    private static partial int hbxl_audio_set_queue(int period, int buffers);

    [LibraryImport("hbxl", EntryPoint = "hbxl_gc_log_set")]
    private static partial void hbxl_gc_log_set(int minPauseMs);

    [LibraryImport("hbxl", EntryPoint = "hbxl_display_set")]
    private static partial int hbxl_display_set(int handheldHeight, int dockedHeight, int fps);

    public static void SetGyro(int button, float speedX, float speedY, bool invertX, bool invertY) =>
        hbxl_input_set_gyro(button, speedX, speedY, invertX ? 1 : 0, invertY ? 1 : 0);

    public static void SetKeyboardMouse(bool enabled, float speed) => hbxl_input_set_kbmouse(enabled ? 1 : 0, speed);

    public static void SetJoyConMouse(int sides, float speed) => hbxl_input_set_joymouse(sides, speed);

    /// <summary>Returns the queue length in microseconds, 0 while the shim's output isn't open.</summary>
    public static int SetAudioQueue(int period, int buffers) => hbxl_audio_set_queue(period, buffers);

    public static void SetGcLog(int minPauseMs) => hbxl_gc_log_set(minPauseMs);

    public static void SetDisplay(int handheldHeight, int dockedHeight, int fps)
    {
        int r = hbxl_display_set(handheldHeight, dockedHeight, fps);
        Program.Log($"switch2 settings: display handheld {handheldHeight}, docked {dockedHeight}, {fps} fps: {r}");
    }
#else
    public static void SetGyro(int button, float speedX, float speedY, bool invertX, bool invertY) =>
        Program.Log($"switch2 settings: gyro button {button}, speed {speedX} / {speedY}, invert {invertX} / {invertY}");

    public static void SetKeyboardMouse(bool enabled, float speed) => Program.Log($"switch2 settings: usb keyboard/mouse {enabled}, mouse speed {speed}");

    public static void SetJoyConMouse(int sides, float speed) => Program.Log($"switch2 settings: joy-con mouse sides {sides}, speed {speed}");

    public static int SetAudioQueue(int period, int buffers)
    {
        Program.Log($"switch2 settings: audio queue {buffers} x {period} frames");
        return (int)((long)period * buffers * 1000000 / 48000);
    }

    public static void SetGcLog(int minPauseMs) => Program.Log($"switch2 settings: gc log {minPauseMs}");

    public static void SetDisplay(int handheldHeight, int dockedHeight, int fps) =>
        Program.Log($"switch2 settings: display handheld {handheldHeight}, docked {dockedHeight}, {fps} fps");
#endif
}

public partial class Switch2Section : SettingsSection
{
    public override LocalisableString Header => "Switch 2";

    public override Drawable CreateIcon() => new SpriteIcon { Icon = FontAwesome.Solid.Gamepad };

    public override IEnumerable<LocalisableString> FilterTerms => base.FilterTerms.Concat(new LocalisableString[] { "console", "hbxl", "nintendo" });

    public Switch2Section(Switch2Config config, Action importNow)
    {
        Children = new Drawable[]
        {
            new DisplaySettings(config),
            new ImportSettings(config, importNow),
            new GyroSettings(config),
            new JoyConMouseSettings(config),
            new KeyboardMouseSettings(config),
            new AudioOutputSettings(config),
            new DiagnosticsSettings(config),
        };
    }

    private static string speed(float v) => v.ToString("0.0", CultureInfo.InvariantCulture) + "x";

    private partial class DisplaySettings : SettingsSubsection
    {
        protected override LocalisableString Header => "Display";

        public DisplaySettings(Switch2Config config)
        {
            Children = new Drawable[]
            {
                new SettingsItemV2(new FormDropdown<string>
                {
                    Caption = "Resolution in handheld mode",
                    HintText = "The screen is 1080p. A lower resolution is scaled up to it: less sharp, less work for the GPU.",
                    Items = Switch2Config.HANDHELD_RESOLUTION,
                    Current = config.GetBindable<string>(Switch2Setting.HandheldResolution),
                })
                {
                    Keywords = new[] { "resolution", "handheld", "screen", "sharp", "blurry" },
                },
                new SettingsItemV2(new FormDropdown<string>
                {
                    Caption = "Resolution when docked",
                    HintText = "Automatic renders at the console's TV output resolution (System Settings > TV Output). Anything else is scaled to that.",
                    Items = Switch2Config.DOCKED_RESOLUTION,
                    Current = config.GetBindable<string>(Switch2Setting.DockedResolution),
                })
                {
                    Keywords = new[] { "resolution", "docked", "tv", "4k", "1440p", "sharp", "blurry" },
                },
                new SettingsItemV2(new FormDropdown<string>
                {
                    Caption = "Frame rate",
                    HintText = "Highest runs at 120 Hz on the handheld screen (and on a TV that takes 120 Hz), with variable refresh rate. Use the Frame limiter setting (Graphics) set to VSync.",
                    Items = Switch2Config.FRAME_RATE,
                    Current = config.GetBindable<string>(Switch2Setting.FrameRate),
                })
                {
                    Keywords = new[] { "fps", "120", "hz", "refresh", "vrr", "framerate" },
                },
            };
        }
    }

    private partial class ImportSettings : SettingsSubsection
    {
        protected override LocalisableString Header => "Import";

        public ImportSettings(Switch2Config config, Action importNow)
        {
            Children = new Drawable[]
            {
                new SettingsButtonV2
                {
                    Text = "Import everything in the import folder now",
                    TooltipText = "Beatmaps (.osz, .olz), skins (.osk) and replays (.osr) in the import folder on the SD card. Imported skins and replays are moved to its imported folder.",
                    Keywords = new[] { "import", "beatmap", "skin", "replay", "osz", "folder" },
                    Action = importNow,
                },
                new SettingsItemV2(new FormCheckBox
                {
                    Caption = "Import new files automatically",
                    HintText = "Looks into the import folder every few seconds (not during gameplay) and imports what is new.",
                    Current = config.GetBindable<bool>(Switch2Setting.ImportAutoCheck),
                })
                {
                    Keywords = new[] { "import", "scan", "folder" },
                },
                new SettingsItemV2(new FormSliderBar<int>
                {
                    Caption = "Look every",
                    Current = config.GetBindable<int>(Switch2Setting.ImportCheckInterval),
                    KeyboardStep = 5,
                    LabelFormat = v => $"{v} s",
                })
                {
                    Keywords = new[] { "import", "scan", "interval" },
                },
            };
        }
    }

    private partial class GyroSettings : SettingsSubsection
    {
        protected override LocalisableString Header => "Gyro aiming";

        public GyroSettings(Switch2Config config)
        {
            Children = new Drawable[]
            {
                new SettingsItemV2(new FormDropdown<string>
                {
                    Caption = "Hold to aim",
                    HintText = "While this button is held, turning the controller moves the cursor. The button then sends no key to osu!.",
                    Items = Switch2Config.GYRO_BUTTONS,
                    Current = config.GetBindable<string>(Switch2Setting.GyroButton),
                })
                {
                    Keywords = new[] { "gyro", "motion", "button" },
                },
                new SettingsItemV2(new FormSliderBar<float>
                {
                    Caption = "Horizontal sensitivity",
                    Current = config.GetBindable<float>(Switch2Setting.GyroSpeedX),
                    KeyboardStep = 0.1f,
                    LabelFormat = v => speed(v),
                })
                {
                    Keywords = new[] { "gyro", "motion", "sensitivity" },
                },
                new SettingsItemV2(new FormSliderBar<float>
                {
                    Caption = "Vertical sensitivity",
                    Current = config.GetBindable<float>(Switch2Setting.GyroSpeedY),
                    KeyboardStep = 0.1f,
                    LabelFormat = v => speed(v),
                })
                {
                    Keywords = new[] { "gyro", "motion", "sensitivity" },
                },
                new SettingsItemV2(new FormCheckBox
                {
                    Caption = "Invert horizontal",
                    Current = config.GetBindable<bool>(Switch2Setting.GyroInvertX),
                })
                {
                    Keywords = new[] { "gyro", "motion" },
                },
                new SettingsItemV2(new FormCheckBox
                {
                    Caption = "Invert vertical",
                    Current = config.GetBindable<bool>(Switch2Setting.GyroInvertY),
                })
                {
                    Keywords = new[] { "gyro", "motion" },
                },
            };
        }
    }

    private partial class JoyConMouseSettings : SettingsSubsection
    {
        protected override LocalisableString Header => "Joy-Con mouse";

        public JoyConMouseSettings(Switch2Config config)
        {
            Children = new Drawable[]
            {
                new SettingsItemV2(new FormDropdown<string>
                {
                    Caption = "Use as a mouse",
                    HintText = "A Joy-Con sliding on a surface moves the cursor; its stick then scrolls and its L/ZL or R/ZR click. Picked up, it is an ordinary controller again.",
                    Items = Switch2Config.JOYCON_MOUSE,
                    Current = config.GetBindable<string>(Switch2Setting.JoyConMouse),
                })
                {
                    Keywords = new[] { "joycon", "mouse", "pointer", "cursor" },
                },
                new SettingsItemV2(new FormSliderBar<float>
                {
                    Caption = "Joy-Con mouse sensitivity",
                    Current = config.GetBindable<float>(Switch2Setting.JoyConMouseSpeed),
                    KeyboardStep = 0.1f,
                    LabelFormat = v => speed(v),
                })
                {
                    Keywords = new[] { "joycon", "mouse", "speed", "cursor" },
                },
            };
        }
    }

    private partial class KeyboardMouseSettings : SettingsSubsection
    {
        protected override LocalisableString Header => "USB keyboard and mouse";

        public KeyboardMouseSettings(Switch2Config config)
        {
            Children = new Drawable[]
            {
                new SettingsItemV2(new FormCheckBox
                {
                    Caption = "Use USB keyboard and mouse",
                    Current = config.GetBindable<bool>(Switch2Setting.UsbKeyboardMouse),
                }),
                new SettingsItemV2(new FormSliderBar<float>
                {
                    Caption = "USB mouse sensitivity",
                    Current = config.GetBindable<float>(Switch2Setting.MouseSpeed),
                    KeyboardStep = 0.1f,
                    LabelFormat = v => speed(v),
                })
                {
                    Keywords = new[] { "speed", "cursor" },
                },
            };
        }
    }

    private partial class AudioOutputSettings : SettingsSubsection
    {
        protected override LocalisableString Header => "Audio output";

        private readonly Bindable<int> period;
        private readonly Bindable<int> buffers;
        private readonly IBindable<double> queueMs;
        private readonly SettingsItemV2 buffersItem;

        public AudioOutputSettings(Switch2Config config)
        {
            period = config.GetBindable<int>(Switch2Setting.AudioPeriod);
            buffers = config.GetBindable<int>(Switch2Setting.AudioBuffers);
            queueMs = config.AudioQueueMs.GetBoundCopy();

            Children = new Drawable[]
            {
                // Applied when the slider is released: each change closes and reopens the console's audio output.
                new SettingsItemV2(new FormSliderBar<int>
                {
                    Caption = "Buffer size",
                    HintText = "Samples per output buffer. Smaller means less delay between a hit and its sound; raise it if the sound crackles.",
                    Current = period,
                    KeyboardStep = 16,
                    TransferValueOnCommit = true,
                    LabelFormat = v => $"{v} samples",
                })
                {
                    Keywords = new[] { "latency", "delay", "period", "crackle" },
                },
                buffersItem = new SettingsItemV2(new FormSliderBar<int>
                {
                    Caption = "Buffers queued",
                    Current = buffers,
                    KeyboardStep = 1,
                    TransferValueOnCommit = true,
                })
                {
                    Keywords = new[] { "latency", "delay", "crackle" },
                },
            };
        }

        protected override void LoadComplete()
        {
            base.LoadComplete();

            period.BindValueChanged(_ => updateNote());
            buffers.BindValueChanged(_ => updateNote());
            queueMs.BindValueChanged(_ => updateNote(), true);
        }

        private void updateNote()
        {
            // What the output really runs with, when it is open; the arithmetic otherwise (e.g. HBXL_AUDIO_SDL=1: not used at all).
            double ms = queueMs.Value > 0 ? queueMs.Value : period.Value * buffers.Value * 1000.0 / 48000;
            string text = $"Output delay: about {ms.ToString("0.0", CultureInfo.InvariantCulture)} ms.";
            buffersItem.Note.Value = new SettingsNote.Data(text, SettingsNote.Type.Informational);
        }
    }

    private partial class DiagnosticsSettings : SettingsSubsection
    {
        protected override LocalisableString Header => "Diagnostics";

        public DiagnosticsSettings(Switch2Config config)
        {
            Children = new Drawable[]
            {
                new SettingsItemV2(new FormCheckBox
                {
                    Caption = "Log every garbage collection",
                    HintText = "One hbxl-gc line per collection in the log.",
                    Current = config.GetBindable<bool>(Switch2Setting.LogGarbageCollections),
                })
                {
                    Keywords = new[] { "gc", "log" },
                },
            };
        }
    }
}
