// RyGT loader: the Controller ports window. Picks which connected controller is player 1 or 2
// (GT Sport's splitscreen), each player's name (the PSN-style user name the game shows), and
// shows a live preview of the chosen controller, like shadPS4.
// Controllers are found and read through Windows' joystick API (winmm), and remembered by USB
// vendor/product id and their place among connected controllers of that id
// (Input.controller_ports in the GT Sport config, read by the renderer's input bridge).
// SPDX-License-Identifier: GPL-2.0-only

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace RyGT
{
    static class Joysticks
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct JoyCaps
        {
            public ushort Mid, Pid;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Name;
            public uint XMin, XMax, YMin, YMax, ZMin, ZMax, NumButtons, PeriodMin, PeriodMax;
            public uint RMin, RMax, UMin, UMax, VMin, VMax, Caps, MaxAxes, NumAxes, MaxButtons;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string RegKey;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string OemVxD;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct JoyInfoEx
        {
            public uint Size, Flags, X, Y, Z, R, U, V, Buttons, ButtonNumber, Pov, Reserved1, Reserved2;
        }

        [DllImport("winmm.dll", CharSet = CharSet.Unicode)]
        static extern int joyGetDevCapsW(IntPtr id, ref JoyCaps caps, int size);

        [DllImport("winmm.dll")]
        static extern int joyGetPosEx(int id, ref JoyInfoEx info);

        public class Device
        {
            public int Id;
            public ushort Vendor, Product;
            public int Nth;
            public string Name;
            public string Key { get { return string.Format("{0:x4}:{1:x4}#{2}", Vendor, Product, Nth); } }
            public bool PlayStation { get { return Vendor == 0x054c; } }
            public bool Xbox { get { return Vendor == 0x045e; } }
        }

        public static string FriendlyName(ushort vendor, ushort product, string fallback)
        {
            if (vendor == 0x054c)
            {
                if (product == 0x0ce6) return "DualSense";
                if (product == 0x0df2) return "DualSense Edge";
                if (product == 0x05c4 || product == 0x09cc || product == 0x0ba0) return "DualShock 4";
                return "PlayStation controller";
            }
            if (vendor == 0x045e) return "Xbox controller";
            if (vendor == 0x057e) return "Nintendo Switch Pro controller";
            string oem = null;
            try
            {
                using (var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(string.Format(
                    @"System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_{0:X4}&PID_{1:X4}", vendor, product)))
                {
                    if (key != null) oem = key.GetValue("OEMName") as string;
                }
            }
            catch { }
            if (!string.IsNullOrEmpty(oem)) return oem;
            return string.IsNullOrEmpty(fallback) || fallback.StartsWith("Microsoft PC-joystick")
                ? string.Format("Controller {0:X4}:{1:X4}", vendor, product) : fallback;
        }

        /// The connected controllers, in Windows' joystick order.
        public static List<Device> Connected()
        {
            var list = new List<Device>();
            var seen = new Dictionary<uint, int>();
            for (int id = 0; id < 16; ++id)
            {
                var info = new JoyInfoEx { Size = (uint)Marshal.SizeOf(typeof(JoyInfoEx)), Flags = 0xff };
                if (joyGetPosEx(id, ref info) != 0) continue;
                var caps = new JoyCaps();
                if (joyGetDevCapsW(new IntPtr(id), ref caps, Marshal.SizeOf(typeof(JoyCaps))) != 0) continue;
                uint key = ((uint)caps.Mid << 16) | caps.Pid;
                int nth;
                seen.TryGetValue(key, out nth);
                seen[key] = nth + 1;
                list.Add(new Device { Id = id, Vendor = caps.Mid, Product = caps.Pid, Nth = nth, Name = FriendlyName(caps.Mid, caps.Pid, caps.Name) });
            }
            return list;
        }

        public static bool Read(int id, out JoyInfoEx info)
        {
            info = new JoyInfoEx { Size = (uint)Marshal.SizeOf(typeof(JoyInfoEx)), Flags = 0xff };
            return joyGetPosEx(id, ref info) == 0;
        }
    }

    /// Which controller the preview draws.
    enum PadKind { DualShock4, DualSense, Xbox, Generic }

    /// A controller drawn with the chosen controller's live state: a DualShock 4, a DualSense or an
    /// Xbox controller (other controllers: the Xbox layout without its colours).
    class ControllerPreview : Control
    {
        public bool Connected;
        public string Caption = "";
        public PadKind Kind = PadKind.DualShock4;
        // The light bar's colour (DualShock 4 and DualSense).
        public Color Light = Color.FromArgb(40, 110, 230);
        // Pressed state by position: Cross is the bottom face button (Xbox A), Circle the right one
        // (B), Square the left one (X), Triangle the top one (Y). Share is View on Xbox, Options is
        // Menu, Ps the PS or Xbox button. Sticks -1..1, triggers 0..1.
        public bool Cross, Circle, Square, Triangle, L1, R1, L3, R3, Share, Options, Ps, Touchpad, PadUp, PadDown, PadLeft, PadRight;
        public float LX, LY, RX, RY, L2, R2;

        public ControllerPreview()
        {
            DoubleBuffered = true;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer, true);
        }

        static readonly Color Body = Color.FromArgb(58, 58, 64);
        // DualSense (white): a near-white shell and touchpad, black sticks and small buttons,
        // clear light grey d-pad and face buttons.
        static readonly Color WhiteBody = Color.FromArgb(242, 242, 244);
        static readonly Color WhiteSeam = Color.FromArgb(168, 168, 176);
        static readonly Color TouchpadSurface = Color.FromArgb(248, 248, 250);
        static readonly Color TouchpadSeam = Color.FromArgb(190, 190, 198);
        static readonly Color ClearButton = Color.FromArgb(218, 218, 224);
        static readonly Color ClearEdge = Color.FromArgb(150, 150, 160);
        static readonly Color ClearSymbol = Color.FromArgb(118, 118, 128);
        static readonly Color Edge = Color.FromArgb(95, 95, 104);
        static readonly Color Off = Color.FromArgb(34, 34, 38);
        static readonly Color On = Color.FromArgb(206, 40, 52);
        static readonly Color Symbol = Color.FromArgb(200, 200, 206);

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(BackColor);
            // Drawn on a 400 x 260 canvas, scaled to fit.
            float scale = Math.Min(Width / 400f, (Height - 24) / 260f);
            g.TranslateTransform((Width - 400 * scale) / 2, 0);
            g.ScaleTransform(scale, scale);
            using (var text = new SolidBrush(Dark.Text))
            using (var font = new Font("Segoe UI", 9f))
            {
                var format = new StringFormat { Alignment = StringAlignment.Center };
                g.DrawString(Caption, font, text, new RectangleF(0, 236, 400, 24), format);
            }
            float alpha = Connected ? 1f : 0.45f;
            Func<Color, Color> fade = c => Color.FromArgb((int)(c.A * alpha), c);
            switch (Kind)
            {
                case PadKind.DualSense: PaintDualSense(g, fade); break;
                case PadKind.Xbox: PaintXbox(g, fade, true); break;
                case PadKind.Generic: PaintXbox(g, fade, false); break;
                default: PaintDualShock4(g, fade); break;
            }
        }

        void PaintDualShock4(Graphics g, Func<Color, Color> fade)
        {
            // Triggers (fill = how far pressed) and bumpers.
            Bar(g, new RectangleF(70, 6, 70, 16), L2, fade);
            Bar(g, new RectangleF(260, 6, 70, 16), R2, fade);
            Pill(g, new RectangleF(66, 26, 78, 12), L1, fade);
            Pill(g, new RectangleF(256, 26, 78, 12), R1, fade);
            // Body: two grips and the middle.
            using (var path = new GraphicsPath())
            {
                path.AddArc(40, 40, 120, 70, 180, 90);
                path.AddLine(100, 40, 300, 40);
                path.AddArc(240, 40, 120, 70, 270, 90);
                path.AddBezier(360, 75, 385, 140, 395, 215, 350, 225);
                path.AddBezier(350, 225, 315, 232, 300, 180, 270, 160);
                path.AddLine(270, 160, 130, 160);
                path.AddBezier(130, 160, 100, 180, 85, 232, 50, 225);
                path.AddBezier(50, 225, 5, 215, 15, 140, 40, 75);
                path.CloseFigure();
                FillBody(g, path, Body, fade);
            }
            // Light bar over the touchpad, touchpad, Share, Options, PS.
            using (var light = new SolidBrush(fade(Light))) g.FillRectangle(light, 150, 40, 100, 3);
            Rounded(g, new RectangleF(140, 44, 120, 58), 8, Touchpad, fade);
            Pill(g, new RectangleF(118, 52, 12, 22), Share, fade);
            Pill(g, new RectangleF(270, 52, 12, 22), Options, fade);
            Round(g, new RectangleF(190, 122, 20, 20), Ps, fade);
            DPad(g, 89, 95, 22, 18, fade);
            // Face buttons, with their symbols in their colours.
            Face(g, 312, 66, Triangle, fade, Color.FromArgb(64, 196, 160), 0);
            Face(g, 312, 120, Cross, fade, Color.FromArgb(120, 150, 230), 1);
            Face(g, 285, 93, Square, fade, Color.FromArgb(220, 130, 190), 2);
            Face(g, 339, 93, Circle, fade, Color.FromArgb(235, 90, 90), 3);
            Stick(g, 140, 150, LX, LY, L3, fade);
            Stick(g, 260, 150, RX, RY, R3, fade);
        }

        void PaintDualSense(Graphics g, Func<Color, Color> fade)
        {
            // Black triggers and bumpers.
            Bar(g, new RectangleF(68, 4, 74, 16), L2, fade);
            Bar(g, new RectangleF(258, 4, 74, 16), R2, fade);
            Pill(g, new RectangleF(62, 24, 82, 12), L1, fade);
            Pill(g, new RectangleF(256, 24, 82, 12), R1, fade);
            // The white shell with its rounder grips.
            using (var path = new GraphicsPath())
            {
                path.AddArc(38, 38, 124, 80, 180, 90);
                path.AddLine(100, 38, 300, 38);
                path.AddArc(238, 38, 124, 80, 270, 90);
                path.AddBezier(362, 78, 392, 140, 400, 222, 352, 230);
                path.AddBezier(352, 230, 318, 236, 300, 200, 274, 184);
                path.AddLine(274, 184, 126, 184);
                path.AddBezier(126, 184, 100, 200, 82, 236, 48, 230);
                path.AddBezier(48, 230, 0, 222, 8, 140, 38, 78);
                path.CloseFigure();
                FillBody(g, path, WhiteBody, WhiteSeam, fade);
            }
            // The white touchpad with the light strips beside it.
            using (var pad = new GraphicsPath())
            {
                var r = new RectangleF(134, 42, 132, 64);
                pad.AddArc(r.X, r.Y, 20, 20, 180, 90);
                pad.AddArc(r.Right - 20, r.Y, 20, 20, 270, 90);
                pad.AddArc(r.Right - 20, r.Bottom - 20, 20, 20, 0, 90);
                pad.AddArc(r.X, r.Bottom - 20, 20, 20, 90, 90);
                pad.CloseFigure();
                FillBody(g, pad, Touchpad ? On : TouchpadSurface, TouchpadSeam, fade);
            }
            using (var pen = new Pen(fade(Light), 3))
            {
                g.DrawLine(pen, 128, 50, 128, 98);
                g.DrawLine(pen, 272, 50, 272, 98);
            }
            // Create, Options, PS and mute (black).
            Pill(g, new RectangleF(110, 48, 10, 22), Share, fade);
            Pill(g, new RectangleF(280, 48, 10, 22), Options, fade);
            Round(g, new RectangleF(190, 120, 20, 20), Ps, fade);
            Pill(g, new RectangleF(191, 146, 18, 7), false, fade);
            // The d-pad and face buttons are clear light grey, with grey symbols.
            DPad(g, 86, 97, 22, 18, fade, ClearButton, ClearEdge);
            Face(g, 316, 70, Triangle, fade, ClearSymbol, 0, ClearButton, ClearEdge);
            Face(g, 316, 124, Cross, fade, ClearSymbol, 1, ClearButton, ClearEdge);
            Face(g, 290, 97, Square, fade, ClearSymbol, 2, ClearButton, ClearEdge);
            Face(g, 342, 97, Circle, fade, ClearSymbol, 3, ClearButton, ClearEdge);
            Stick(g, 148, 156, LX, LY, L3, fade);
            Stick(g, 252, 156, RX, RY, R3, fade);
        }

        void PaintXbox(Graphics g, Func<Color, Color> fade, bool colours)
        {
            // LT/RT and LB/RB.
            Bar(g, new RectangleF(72, 8, 70, 16), L2, fade);
            Bar(g, new RectangleF(258, 8, 70, 16), R2, fade);
            Pill(g, new RectangleF(60, 30, 86, 13), L1, fade);
            Pill(g, new RectangleF(254, 30, 86, 13), R1, fade);
            using (var path = new GraphicsPath())
            {
                path.AddArc(40, 45, 140, 80, 180, 90);
                path.AddLine(110, 45, 290, 45);
                path.AddArc(220, 45, 140, 80, 270, 90);
                path.AddBezier(360, 85, 392, 150, 392, 225, 345, 230);
                path.AddBezier(345, 230, 310, 234, 292, 196, 262, 176);
                path.AddLine(262, 176, 138, 176);
                path.AddBezier(138, 176, 108, 196, 90, 234, 55, 230);
                path.AddBezier(55, 230, 8, 225, 8, 150, 40, 85);
                path.CloseFigure();
                FillBody(g, path, Body, fade);
            }
            // Xbox button (with its X), View, Menu, Share.
            Round(g, new RectangleF(186, 56, 28, 28), Ps, fade);
            using (var pen = new Pen(fade(Ps ? Color.White : Symbol), 2))
            {
                g.DrawLine(pen, 194, 64, 206, 76);
                g.DrawLine(pen, 206, 64, 194, 76);
            }
            Rounded(g, new RectangleF(160, 96, 18, 12), 5, Share, fade);
            Rounded(g, new RectangleF(222, 96, 18, 12), 5, Options, fade);
            Rounded(g, new RectangleF(193, 112, 14, 9), 4, false, fade);
            // Left stick high, d-pad low; right stick low.
            Stick(g, 116, 96, LX, LY, L3, fade);
            DPad(g, 152, 144, 20, 16, fade);
            Stick(g, 248, 148, RX, RY, R3, fade);
            // A B X Y.
            Letter(g, 300, 120, Cross, "A", Color.FromArgb(96, 184, 72), colours, fade);
            Letter(g, 324, 96, Circle, "B", Color.FromArgb(225, 64, 64), colours, fade);
            Letter(g, 276, 96, Square, "X", Color.FromArgb(64, 132, 228), colours, fade);
            Letter(g, 300, 72, Triangle, "Y", Color.FromArgb(236, 196, 48), colours, fade);
        }

        static void FillBody(Graphics g, GraphicsPath path, Color colour, Func<Color, Color> fade)
        {
            FillBody(g, path, colour, Edge, fade);
        }

        static void FillBody(Graphics g, GraphicsPath path, Color colour, Color seam, Func<Color, Color> fade)
        {
            using (var fill = new SolidBrush(fade(colour))) g.FillPath(fill, path);
            using (var pen = new Pen(fade(seam), 2)) g.DrawPath(pen, path);
        }

        void DPad(Graphics g, float cx, float cy, float length, float width, Func<Color, Color> fade)
        {
            DPad(g, cx, cy, length, width, fade, Off, Edge);
        }

        void DPad(Graphics g, float cx, float cy, float length, float width, Func<Color, Color> fade, Color fill, Color edge)
        {
            float h = width / 2;
            Rounded(g, new RectangleF(cx - h, cy - h - length, width, length), 3, PadUp, fade, fill, edge);
            Rounded(g, new RectangleF(cx - h, cy + h, width, length), 3, PadDown, fade, fill, edge);
            Rounded(g, new RectangleF(cx - h - length, cy - h, length, width), 3, PadLeft, fade, fill, edge);
            Rounded(g, new RectangleF(cx + h, cy - h, length, width), 3, PadRight, fade, fill, edge);
        }

        static void Letter(Graphics g, float x, float y, bool pressed, string letter, Color colour, bool colours, Func<Color, Color> fade)
        {
            using (var fill = new SolidBrush(fade(pressed ? (colours ? colour : On) : Off))) g.FillEllipse(fill, x - 12, y - 12, 24, 24);
            using (var pen = new Pen(fade(Edge))) g.DrawEllipse(pen, x - 12, y - 12, 24, 24);
            using (var font = new Font("Segoe UI", 9.5f, FontStyle.Bold))
            using (var text = new SolidBrush(fade(pressed ? Color.White : (colours ? colour : Symbol))))
            {
                var format = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center };
                g.DrawString(letter, font, text, new RectangleF(x - 12, y - 11, 24, 24), format);
            }
        }

        static void Bar(Graphics g, RectangleF r, float amount, Func<Color, Color> fade)
        {
            using (var off = new SolidBrush(fade(Off))) g.FillRectangle(off, r);
            var filled = new RectangleF(r.X, r.Y, r.Width * Math.Max(0, Math.Min(1, amount)), r.Height);
            using (var on = new SolidBrush(fade(On))) g.FillRectangle(on, filled);
            using (var pen = new Pen(fade(Edge))) g.DrawRectangle(pen, r.X, r.Y, r.Width, r.Height);
        }

        static void Pill(Graphics g, RectangleF r, bool pressed, Func<Color, Color> fade)
        {
            Rounded(g, r, Math.Min(r.Width, r.Height) / 2, pressed, fade);
        }

        static void Rounded(Graphics g, RectangleF r, float radius, bool pressed, Func<Color, Color> fade)
        {
            Rounded(g, r, radius, pressed, fade, Off, Edge);
        }

        static void Rounded(Graphics g, RectangleF r, float radius, bool pressed, Func<Color, Color> fade, Color fill, Color edge)
        {
            using (var path = new GraphicsPath())
            {
                float d = radius * 2;
                path.AddArc(r.X, r.Y, d, d, 180, 90);
                path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
                path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
                path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
                path.CloseFigure();
                using (var brush = new SolidBrush(fade(pressed ? On : fill))) g.FillPath(brush, path);
                using (var pen = new Pen(fade(edge))) g.DrawPath(pen, path);
            }
        }

        static void Round(Graphics g, RectangleF r, bool pressed, Func<Color, Color> fade)
        {
            Round(g, r, pressed, fade, Off, Edge);
        }

        static void Round(Graphics g, RectangleF r, bool pressed, Func<Color, Color> fade, Color fill, Color edge)
        {
            using (var brush = new SolidBrush(fade(pressed ? On : fill))) g.FillEllipse(brush, r);
            using (var pen = new Pen(fade(edge))) g.DrawEllipse(pen, r);
        }

        static void Face(Graphics g, float x, float y, bool pressed, Func<Color, Color> fade, Color symbol, int kind)
        {
            Face(g, x, y, pressed, fade, symbol, kind, Off, Edge);
        }

        static void Face(Graphics g, float x, float y, bool pressed, Func<Color, Color> fade, Color symbol, int kind, Color fill, Color edge)
        {
            Round(g, new RectangleF(x - 12, y - 12, 24, 24), pressed, fade, fill, edge);
            using (var pen = new Pen(fade(pressed ? Color.White : symbol), 2))
            {
                switch (kind)
                {
                    case 0: g.DrawPolygon(pen, new[] { new PointF(x, y - 6), new PointF(x + 6, y + 5), new PointF(x - 6, y + 5) }); break;
                    case 1: g.DrawLine(pen, x - 5, y - 5, x + 5, y + 5); g.DrawLine(pen, x + 5, y - 5, x - 5, y + 5); break;
                    case 2: g.DrawRectangle(pen, x - 5, y - 5, 10, 10); break;
                    default: g.DrawEllipse(pen, x - 6, y - 6, 12, 12); break;
                }
            }
        }

        static void Stick(Graphics g, float x, float y, float sx, float sy, bool pressed, Func<Color, Color> fade)
        {
            using (var fill = new SolidBrush(fade(Off))) g.FillEllipse(fill, x - 24, y - 24, 48, 48);
            using (var pen = new Pen(fade(Edge))) g.DrawEllipse(pen, x - 24, y - 24, 48, 48);
            float cx = x + sx * 12, cy = y + sy * 12;
            Round(g, new RectangleF(cx - 14, cy - 14, 28, 28), pressed, fade);
        }

        /// The layout for a controller, by its USB ids.
        public static PadKind KindOf(ushort vendor, ushort product)
        {
            if (vendor == 0x054c) return product == 0x0ce6 || product == 0x0df2 ? PadKind.DualSense : PadKind.DualShock4;
            if (vendor == 0x045e) return PadKind.Xbox;
            return PadKind.Generic;
        }
    }

    class PortsForm : Form
    {
        // GT Sport's Split Screen is for 2 players.
        public const int Players = 2;
        // The game's user names are at most 16 bytes (UTF-8).
        public const int MaxNameBytes = 16;
        readonly ComboBox[] ports = new ComboBox[Players];
        readonly TextBox[] names = new TextBox[Players];
        readonly Label[] status = new Label[Players];
        readonly ControllerPreview preview;
        readonly Timer timer;
        readonly Action<string, string[]> save;
        List<Joysticks.Device> devices = new List<Joysticks.Device>();
        readonly string[] chosen = new string[Players];
        int shownPlayer;
        int rescanTicks;

        public PortsForm(string current, string[] currentNames, Action<string, string[]> save)
        {
            this.save = save;
            var parts = (current ?? "").Split(',');
            for (int i = 0; i < Players; ++i) chosen[i] = i < parts.Length ? parts[i].Trim() : "";

            Text = "ryGT - Controller ports";
            Font = new Font("Segoe UI", 9f);
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(900, 540);
            MinimumSize = new Size(760, 420);
            MinimizeBox = false;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            var outer = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 4, Padding = new Padding(18, 14, 18, 14) };
            outer.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 400));
            outer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            outer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            outer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            outer.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            outer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            Controls.Add(outer);
            var heading = new Label { Text = "Controller ports", AutoSize = true, Font = new Font("Segoe UI Semibold", 16f), Margin = new Padding(0, 0, 0, 4) };
            outer.Controls.Add(heading, 0, 0);
            outer.SetColumnSpan(heading, 2);

            var intro = new Label
            {
                AutoSize = true, MaximumSize = new Size(840, 0), Margin = new Padding(0, 0, 0, 12), ForeColor = Dark.DimText, Tag = "heading",
                Text = "Pick each player's name and controller (GT Sport: Arcade, Split Screen for 2 players). " +
                       "Names go into new saves (GT Sport keeps the name a save was made with). " +
                       "In game, F3 or holding Share + Options (View + Menu) changes controllers. " +
                       "\"Next connected\" takes the controllers nobody picked, in the order Windows lists them. " +
                       "Press buttons to see them light up in the preview; click a player to preview their controller."
            };
            outer.Controls.Add(intro, 0, 1);
            outer.SetColumnSpan(intro, 2);

            var table = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Dock = DockStyle.Top, Margin = new Padding(0) };
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            for (int i = 0; i < Players; ++i)
            {
                int player = i;
                table.Controls.Add(new Label { Text = "Player " + (i + 1), AutoSize = true, Margin = new Padding(0, 9, 10, 0), Font = new Font("Segoe UI Semibold", 9.5f) }, 0, i * 3);
                var nameRow = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 4, 0, 0) };
                nameRow.Controls.Add(new Label { Text = "Name", AutoSize = true, Margin = new Padding(0, 4, 8, 0) });
                names[i] = new TextBox { Width = 130, BorderStyle = BorderStyle.FixedSingle, MaxLength = MaxNameBytes,
                                         Text = currentNames != null && i < currentNames.Length ? currentNames[i] ?? "" : "" };
                names[i].Enter += delegate { shownPlayer = player; };
                nameRow.Controls.Add(names[i]);
                nameRow.Controls.Add(new Label { Text = i == 0 ? "empty: Player" : "empty: Player " + (i + 1), AutoSize = true, Margin = new Padding(6, 4, 0, 0), ForeColor = Dark.DimText, Tag = "heading" });
                table.Controls.Add(nameRow, 1, i * 3);
                ports[i] = new DarkComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill, FlatStyle = FlatStyle.Flat, Margin = new Padding(0, 4, 0, 0) };
                ports[i].SelectedIndexChanged += delegate { OnPick(player); };
                ports[i].Enter += delegate { shownPlayer = player; };
                ports[i].DropDown += delegate { shownPlayer = player; };
                table.Controls.Add(ports[i], 1, i * 3 + 1);
                status[i] = new Label { AutoSize = true, ForeColor = Dark.DimText, Tag = "heading", Margin = new Padding(0, 2, 0, 10) };
                table.Controls.Add(status[i], 1, i * 3 + 2);
            }
            var players = new Card(Dark.Panel) { Dock = DockStyle.Fill, Padding = new Padding(16, 12, 16, 10), Margin = new Padding(0) };
            players.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            players.Controls.Add(table);
            outer.Controls.Add(players, 0, 2);

            preview = new ControllerPreview { Dock = DockStyle.Fill, Margin = new Padding(0), MinimumSize = new Size(300, 220) };
            var previewCard = new Card(Dark.Panel) { Dock = DockStyle.Fill, Padding = new Padding(12, 16, 12, 8), Margin = new Padding(14, 0, 0, 0) };
            previewCard.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            previewCard.Controls.Add(preview);
            outer.Controls.Add(previewCard, 1, 2);

            var buttons = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 12, 0, 0) };
            var ok = new Button { Text = "Save", AutoSize = true, Tag = "accent" };
            var reset = new Button { Text = "All automatic", AutoSize = true };
            var cancel = new Button { Text = "Cancel", AutoSize = true, DialogResult = DialogResult.Cancel };
            ok.Click += delegate
            {
                var picked = new string[Players];
                for (int i = 0; i < Players; ++i)
                {
                    picked[i] = names[i].Text.Trim();
                    if (System.Text.Encoding.UTF8.GetByteCount(picked[i]) > MaxNameBytes)
                    {
                        MessageBox.Show(this, "Player " + (i + 1) + "'s name is too long: up to " + MaxNameBytes + " letters (fewer with accents or symbols).");
                        names[i].Focus();
                        return;
                    }
                }
                try { save(string.Join(",", chosen), picked); DialogResult = DialogResult.OK; Close(); }
                catch (Exception e) { MessageBox.Show(this, "Could not save the ports: " + e.Message); }
            };
            reset.Click += delegate { for (int i = 0; i < Players; ++i) chosen[i] = ""; Fill(); };
            buttons.Controls.AddRange(new Control[] { ok, reset, cancel });
            outer.Controls.Add(buttons, 0, 3);
            outer.SetColumnSpan(buttons, 2);
            CancelButton = cancel;

            Dark.Apply(this);
            preview.BackColor = Dark.Panel;
            HandleCreated += delegate { Dark.TitleBar(this); };

            devices = Joysticks.Connected();
            Fill();
            timer = new Timer { Interval = 33 };
            timer.Tick += delegate { Tick(); };
            timer.Start();
            FormClosed += delegate { timer.Stop(); timer.Dispose(); };
        }

        bool filling;

        /// The combo boxes from the connected controllers and the choices.
        void Fill()
        {
            filling = true;
            for (int i = 0; i < Players; ++i)
            {
                var box = ports[i];
                box.Items.Clear();
                box.Items.Add("Next connected (automatic)");
                foreach (var d in devices) box.Items.Add(DeviceLabel(d));
                int index = 0;
                if (!string.IsNullOrEmpty(chosen[i]))
                {
                    int found = devices.FindIndex(d => d.Key == chosen[i]);
                    if (found >= 0) index = found + 1;
                    else { box.Items.Add("Not connected: " + chosen[i]); index = box.Items.Count - 1; }
                }
                box.SelectedIndex = index;
            }
            filling = false;
            UpdateStatus();
        }

        string DeviceLabel(Joysticks.Device d)
        {
            int same = devices.Count(x => x.Vendor == d.Vendor && x.Product == d.Product);
            return same > 1 ? string.Format("{0} ({1})", d.Name, d.Nth + 1) : d.Name;
        }

        void OnPick(int player)
        {
            if (filling) return;
            shownPlayer = player;
            int index = ports[player].SelectedIndex;
            if (index <= 0) chosen[player] = "";
            else if (index <= devices.Count)
            {
                var key = devices[index - 1].Key;
                // A controller plays as one player: picking it here frees it elsewhere.
                for (int i = 0; i < Players; ++i) if (i != player && chosen[i] == key) chosen[i] = "";
                chosen[player] = key;
            }
            Fill();
        }

        /// Which controller each player gets, as the game will assign them: the picked ones
        /// first, then the others in Windows' order.
        Joysticks.Device[] Assignment()
        {
            var result = new Joysticks.Device[Players];
            var used = new HashSet<string>();
            for (int i = 0; i < Players; ++i)
            {
                var d = devices.FirstOrDefault(x => x.Key == chosen[i]);
                if (d != null) { result[i] = d; used.Add(d.Key); }
            }
            var rest = devices.Where(d => !used.Contains(d.Key)).ToList();
            for (int i = 0; i < Players && rest.Count > 0; ++i)
            {
                if (result[i] == null) { result[i] = rest[0]; rest.RemoveAt(0); }
            }
            return result;
        }

        void UpdateStatus()
        {
            var assigned = Assignment();
            for (int i = 0; i < Players; ++i)
            {
                status[i].Text = assigned[i] != null ? "Plays with: " + DeviceLabel(assigned[i]) :
                                 i == 0 ? "Plays with: the keyboard (no controller)" : "No controller: this player stays out";
            }
        }

        void Tick()
        {
            if (++rescanTicks >= 60)
            {
                rescanTicks = 0;
                var now = Joysticks.Connected();
                if (!now.Select(d => d.Key + d.Id).SequenceEqual(devices.Select(d => d.Key + d.Id)))
                {
                    devices = now;
                    Fill();
                }
            }
            // A controller with a button held shows up in the preview too.
            var assigned = Assignment();
            var device = assigned[shownPlayer];
            Joysticks.JoyInfoEx info;
            preview.Kind = device != null ? ControllerPreview.KindOf(device.Vendor, device.Product) : PadKind.DualShock4;
            preview.Connected = device != null && Joysticks.Read(device.Id, out info) && Apply(device, info);
            if (!preview.Connected) Clear();
            preview.Caption = "Player " + (shownPlayer + 1) + ": " + (device != null ? DeviceLabel(device) : "no controller");
            preview.Invalidate();
        }

        void Clear()
        {
            preview.Cross = preview.Circle = preview.Square = preview.Triangle = preview.L1 = preview.R1 = false;
            preview.L3 = preview.R3 = preview.Share = preview.Options = preview.Ps = preview.Touchpad = false;
            preview.PadUp = preview.PadDown = preview.PadLeft = preview.PadRight = false;
            preview.LX = preview.LY = preview.RX = preview.RY = preview.L2 = preview.R2 = 0;
        }

        static float Axis(uint value) { return (value - 32767.5f) / 32767.5f; }

        bool Apply(Joysticks.Device device, Joysticks.JoyInfoEx info)
        {
            Func<int, bool> b = n => (info.Buttons & (1u << n)) != 0;
            uint pov = info.Pov;
            bool povActive = pov != 0xffff && pov <= 36000;
            preview.PadUp = povActive && (pov >= 31500 || pov <= 4500);
            preview.PadRight = povActive && pov >= 4500 && pov <= 13500;
            preview.PadDown = povActive && pov >= 13500 && pov <= 22500;
            preview.PadLeft = povActive && pov >= 22500 && pov <= 31500;
            preview.LX = Axis(info.X);
            preview.LY = Axis(info.Y);
            if (device.Xbox)
            {
                // XInput through the joystick API: A B X Y LB RB Back Start LS RS; both triggers on Z.
                preview.Cross = b(0); preview.Circle = b(1); preview.Square = b(2); preview.Triangle = b(3);
                preview.L1 = b(4); preview.R1 = b(5); preview.Share = b(6); preview.Options = b(7);
                preview.L3 = b(8); preview.R3 = b(9); preview.Ps = false; preview.Touchpad = false;
                float z = Axis(info.Z);
                preview.L2 = Math.Max(0, z);
                preview.R2 = Math.Max(0, -z);
                preview.RX = Axis(info.U);
                preview.RY = Axis(info.R);
            }
            else
            {
                // DualShock 4, DualSense and most other pads: Square Cross Circle Triangle L1 R1 L2 R2
                // Share Options L3 R3 PS Touchpad; right stick on Z/R, triggers on U/V.
                preview.Square = b(0); preview.Cross = b(1); preview.Circle = b(2); preview.Triangle = b(3);
                preview.L1 = b(4); preview.R1 = b(5);
                preview.Share = b(8); preview.Options = b(9); preview.L3 = b(10); preview.R3 = b(11);
                preview.Ps = b(12); preview.Touchpad = b(13);
                preview.RX = Axis(info.Z);
                preview.RY = Axis(info.R);
                preview.L2 = Math.Max(b(6) ? 1f : 0f, (Axis(info.U) + 1) / 2);
                preview.R2 = Math.Max(b(7) ? 1f : 0f, (Axis(info.V) + 1) / 2);
                if (!device.PlayStation && info.U == 0 && info.V == 0) { preview.L2 = b(6) ? 1 : 0; preview.R2 = b(7) ? 1 : 0; }
            }
            return true;
        }
    }
}
