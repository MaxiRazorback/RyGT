// RyGT loader: the controller and keyboard bindings window (like shadPS4's controller settings).
// It edits user\input_config\default.ini, the bindings file shadGT reads (unified input config),
// and keeps every line it does not manage (hotkeys, mouse bindings, colours) as it is.
// SPDX-License-Identifier: GPL-2.0-only

using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows.Forms;

namespace RyGT
{
    class ControlsForm : Form
    {
        /// One PS4 control: its name in the bindings file, its label, and whether a controller
        /// stick axis (rather than a button) is bound to it.
        class Row
        {
            public string Output, Label;
            public bool Axis, KeyboardOnly, ControllerOnly;
            public ComboBox Pad, Key, AltKey;
        }

        static readonly string[] PadButtons =
        {
            "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3", "options", "back",
            "pad_up", "pad_down", "pad_left", "pad_right", "lpaddle_high", "lpaddle_low", "rpaddle_high", "rpaddle_low", "qam"
        };
        static readonly string[] PadAxes = { "axis_left_x", "axis_left_y", "axis_right_x", "axis_right_y" };
        static readonly string[] Keys =
        {
            "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
            "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "space", "enter", "escape", "backspace", "tab", "capslock",
            "lshift", "rshift", "lctrl", "rctrl", "lalt", "ralt", "up", "down", "left", "right",
            "insert", "delete", "home", "end", "pgup", "pgdown",
            "kp0", "kp1", "kp2", "kp3", "kp4", "kp5", "kp6", "kp7", "kp8", "kp9", "kpperiod", "kpslash",
            "f1", "f2", "f3", "f4", "f5", "f6", "f7",
            "grave", "minus", "equals", "lbracket", "rbracket", "backslash", "semicolon", "apostrophe", "comma", "period", "slash"
        };
        const string None = "(none)";

        static readonly Row[] Rows =
        {
            new Row { Output = "cross", Label = "Cross" },
            new Row { Output = "circle", Label = "Circle" },
            new Row { Output = "square", Label = "Square" },
            new Row { Output = "triangle", Label = "Triangle" },
            new Row { Output = "l1", Label = "L1" },
            new Row { Output = "r1", Label = "R1" },
            new Row { Output = "l2", Label = "L2 (brake)" },
            new Row { Output = "r2", Label = "R2 (accelerate)" },
            new Row { Output = "l3", Label = "L3" },
            new Row { Output = "r3", Label = "R3" },
            new Row { Output = "options", Label = "Options" },
            new Row { Output = "touchpad_center", Label = "Touchpad" },
            new Row { Output = "pad_up", Label = "D-pad up" },
            new Row { Output = "pad_down", Label = "D-pad down" },
            new Row { Output = "pad_left", Label = "D-pad left" },
            new Row { Output = "pad_right", Label = "D-pad right" },
            new Row { Output = "axis_left_x", Label = "Left stick, left/right (steering)", Axis = true, ControllerOnly = true },
            new Row { Output = "axis_left_y", Label = "Left stick, up/down", Axis = true, ControllerOnly = true },
            new Row { Output = "axis_right_x", Label = "Right stick, left/right", Axis = true, ControllerOnly = true },
            new Row { Output = "axis_right_y", Label = "Right stick, up/down", Axis = true, ControllerOnly = true },
            new Row { Output = "axis_left_x_minus", Label = "Steer left (left stick left)", KeyboardOnly = true },
            new Row { Output = "axis_left_x_plus", Label = "Steer right (left stick right)", KeyboardOnly = true },
            new Row { Output = "axis_left_y_minus", Label = "Left stick up", KeyboardOnly = true },
            new Row { Output = "axis_left_y_plus", Label = "Left stick down", KeyboardOnly = true },
            new Row { Output = "axis_right_x_minus", Label = "Right stick left", KeyboardOnly = true },
            new Row { Output = "axis_right_x_plus", Label = "Right stick right", KeyboardOnly = true },
            new Row { Output = "axis_right_y_minus", Label = "Right stick up", KeyboardOnly = true },
            new Row { Output = "axis_right_y_plus", Label = "Right stick down", KeyboardOnly = true },
        };

        // shadGT's default bindings (input_handler.cpp GetDefaultInputConfig).
        static readonly Dictionary<string, string[]> DefaultKeys = new Dictionary<string, string[]>
        {
            { "triangle", new[] { "kp8", "c" } }, { "circle", new[] { "kp6", "b" } }, { "cross", new[] { "kp2", "n" } },
            { "square", new[] { "kp4", "v" } }, { "l1", new[] { "q" } }, { "r1", new[] { "u" } }, { "l2", new[] { "e" } },
            { "r2", new[] { "o" } }, { "l3", new[] { "x" } }, { "r3", new[] { "m" } }, { "options", new[] { "enter" } },
            { "touchpad_center", new[] { "space" } }, { "pad_up", new[] { "up" } }, { "pad_down", new[] { "down" } },
            { "pad_left", new[] { "left" } }, { "pad_right", new[] { "right" } },
            { "axis_left_x_minus", new[] { "a" } }, { "axis_left_x_plus", new[] { "d" } },
            { "axis_left_y_minus", new[] { "w" } }, { "axis_left_y_plus", new[] { "s" } },
            { "axis_right_x_minus", new[] { "j" } }, { "axis_right_x_plus", new[] { "l" } },
            { "axis_right_y_minus", new[] { "i" } }, { "axis_right_y_plus", new[] { "k" } },
        };

        readonly string path;
        readonly NumericUpDown leftDeadzone, rightDeadzone;
        readonly ComboBox keyboardPlayer;
        readonly Action<int> saveKeyboardPlayer;

        public ControlsForm(string root, int currentKeyboardPlayer, Action<int> saveKeyboardPlayer)
        {
            this.saveKeyboardPlayer = saveKeyboardPlayer;
            path = Path.Combine(root, "user", "input_config", "default.ini");
            Text = "ryGT - Controls";
            Font = new Font("Segoe UI", 9f);
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(780, 760);
            MinimumSize = new Size(640, 480);
            FormBorderStyle = FormBorderStyle.Sizable;
            MinimizeBox = false;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            var outer = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, Padding = new Padding(18, 14, 18, 14) };
            outer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            Controls.Add(outer);
            outer.Controls.Add(new Label { Text = "Button bindings", AutoSize = true, Font = new Font("Segoe UI Semibold", 16f), Margin = new Padding(0, 0, 0, 4) });

            outer.Controls.Add(new Label
            {
                AutoSize = true, MaximumSize = new Size(720, 0), Margin = new Padding(0, 0, 0, 10), ForeColor = Dark.DimText, Tag = "heading",
                Text = "Pick what each PS4 control is on your controller and keyboard. Every controller uses the " +
                       "controller bindings. Which controller is which player (splitscreen) is set under Controller ports."
            });

            var players = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 0, 0, 6) };
            players.Controls.Add(new Label { Text = "The keyboard controls", AutoSize = true, Margin = new Padding(0, 6, 8, 0) });
            keyboardPlayer = new DarkComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 120, FlatStyle = FlatStyle.Flat };
            keyboardPlayer.Items.AddRange(new object[] { "Player 1", "Player 2" });
            keyboardPlayer.SelectedIndex = Math.Max(0, Math.Min(PortsForm.Players - 1, currentKeyboardPlayer - 1));
            players.Controls.Add(keyboardPlayer);
            players.Controls.Add(new Label { Text = "(player 2 on the keyboard: no second controller needed)", AutoSize = true, Margin = new Padding(8, 6, 0, 0), ForeColor = Dark.DimText, Tag = "heading" });
            outer.Controls.Add(players);

            var table = new TableLayoutPanel { ColumnCount = 4, AutoSize = true, Dock = DockStyle.Top, Margin = new Padding(0) };
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 40));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 20));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 20));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 20));
            foreach (var heading in new[] { "PS4 control", "Controller", "Key", "Second key" })
            {
                table.Controls.Add(new Label { Text = heading.ToUpperInvariant(), AutoSize = true, Font = new Font("Segoe UI Semibold", 8.5f), ForeColor = Dark.Heading, Tag = "heading", Margin = new Padding(0, 0, 0, 4) });
            }
            foreach (var row in Rows)
            {
                table.Controls.Add(new Label { Text = row.Label, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 5, 6, 0) });
                row.Pad = Combo(row.KeyboardOnly ? null : (row.Axis ? PadAxes : PadButtons));
                row.Key = Combo(row.ControllerOnly ? null : Keys);
                row.AltKey = Combo(row.ControllerOnly ? null : Keys);
                table.Controls.Add(row.Pad);
                table.Controls.Add(row.Key);
                table.Controls.Add(row.AltKey);
            }
            var scroll = new Panel { Dock = DockStyle.Fill, AutoScroll = true, Margin = new Padding(0) };
            scroll.Controls.Add(table);
            var bindings = new Card(Dark.Panel) { Dock = DockStyle.Fill, Padding = new Padding(16, 12, 6, 10), Margin = new Padding(0, 4, 0, 4) };
            bindings.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            bindings.Controls.Add(scroll);
            outer.Controls.Add(bindings);

            var deadzones = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 8, 0, 4) };
            deadzones.Controls.Add(new Label { Text = "Stick deadzone (1-127): left", AutoSize = true, Margin = new Padding(0, 6, 6, 0) });
            leftDeadzone = new NumericUpDown { Minimum = 1, Maximum = 127, Value = 5, Width = 60 };
            deadzones.Controls.Add(leftDeadzone);
            deadzones.Controls.Add(new Label { Text = "right", AutoSize = true, Margin = new Padding(12, 6, 6, 0) });
            rightDeadzone = new NumericUpDown { Minimum = 1, Maximum = 127, Value = 5, Width = 60 };
            deadzones.Controls.Add(rightDeadzone);
            outer.Controls.Add(deadzones);

            var buttons = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 6, 0, 0) };
            var save = new Button { Text = "Save", AutoSize = true, Tag = "accent" };
            var defaults = new Button { Text = "Restore defaults", AutoSize = true };
            var cancel = new Button { Text = "Cancel", AutoSize = true, DialogResult = DialogResult.Cancel };
            save.Click += delegate
            {
                try { Save(); DialogResult = DialogResult.OK; Close(); }
                catch (Exception e) { MessageBox.Show(this, "Could not save the controls: " + e.Message); }
            };
            defaults.Click += delegate { LoadBindings(null); };
            buttons.Controls.AddRange(new Control[] { save, defaults, cancel });
            outer.Controls.Add(buttons);
            CancelButton = cancel;

            outer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            outer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            outer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            outer.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            outer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            outer.RowStyles.Add(new RowStyle(SizeType.AutoSize));

            LoadBindings(File.Exists(path) ? File.ReadAllLines(path) : null);
            Dark.Apply(this);
            HandleCreated += delegate { Dark.TitleBar(this); };
        }

        static ComboBox Combo(string[] items)
        {
            var box = new DarkComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill, FlatStyle = FlatStyle.Flat, Margin = new Padding(0, 2, 6, 2) };
            if (items == null) { box.Enabled = false; return box; }
            box.Items.Add(None);
            box.Items.AddRange(items);
            box.SelectedIndex = 0;
            return box;
        }

        static void Select(ComboBox box, string value)
        {
            if (!box.Enabled) return;
            int index = value == null ? 0 : box.Items.IndexOf(value);
            box.SelectedIndex = index >= 0 ? index : 0;
        }

        /// A binding line, "output[:player] = input[:controller]" with # comments, split apart.
        static bool Parse(string line, out string output, out string input)
        {
            output = input = null;
            var text = line;
            int hash = text.IndexOf('#');
            if (hash >= 0) text = text.Substring(0, hash);
            text = new string(text.Where(c => !char.IsWhiteSpace(c)).ToArray());
            int eq = text.IndexOf('=');
            if (eq <= 0) return false;
            output = text.Substring(0, eq);
            input = text.Substring(eq + 1);
            int colon = output.IndexOf(':');
            if (colon >= 0) output = output.Substring(0, colon);
            return true;
        }

        static bool IsManaged(string output)
        {
            return Rows.Any(r => r.Output == output) || output == "analog_deadzone";
        }

        void LoadBindings(string[] lines)
        {
            var pad = new Dictionary<string, string>();
            var keys = new Dictionary<string, List<string>>();
            if (lines == null)
            {
                foreach (var row in Rows)
                {
                    if (!row.KeyboardOnly) pad[row.Output] = row.Output == "touchpad_center" ? "back" : row.Output;
                    string[] defaultKeys;
                    if (DefaultKeys.TryGetValue(row.Output, out defaultKeys)) keys[row.Output] = defaultKeys.ToList();
                }
                leftDeadzone.Value = rightDeadzone.Value = 5;
            }
            else
            {
                foreach (var line in lines)
                {
                    string output, input;
                    if (!Parse(line, out output, out input)) continue;
                    if (output == "analog_deadzone")
                    {
                        var parts = input.Split(',');
                        int value;
                        if (parts.Length >= 2 && int.TryParse(parts[1], out value))
                        {
                            value = Math.Max(1, Math.Min(127, value));
                            if (parts[0] == "leftjoystick") leftDeadzone.Value = value;
                            if (parts[0] == "rightjoystick") rightDeadzone.Value = value;
                        }
                        continue;
                    }
                    if (!Rows.Any(r => r.Output == output) || input.Contains(',')) continue;
                    int colon = input.IndexOf(':');
                    var name = colon >= 0 ? input.Substring(0, colon) : input;
                    if (Keys.Contains(name))
                    {
                        List<string> list;
                        if (!keys.TryGetValue(output, out list)) keys[output] = list = new List<string>();
                        if (list.Count < 2) list.Add(name);
                    }
                    else if (PadButtons.Contains(name) || PadAxes.Contains(name))
                    {
                        if (!pad.ContainsKey(output)) pad[output] = name;
                    }
                }
            }
            foreach (var row in Rows)
            {
                string padName;
                Select(row.Pad, pad.TryGetValue(row.Output, out padName) ? padName : null);
                List<string> list;
                keys.TryGetValue(row.Output, out list);
                Select(row.Key, list != null && list.Count > 0 ? list[0] : null);
                Select(row.AltKey, list != null && list.Count > 1 ? list[1] : null);
            }
        }

        void Save()
        {
            int player = keyboardPlayer.SelectedIndex + 1;
            string keyboardTarget = player > 1 ? ":" + player : "";
            var text = new StringBuilder();
            text.AppendLine("# Made by RyGT.exe (Controls). Lines it does not manage are kept at the end.");
            text.AppendLine();
            text.AppendLine("# Keyboard bindings" + (player > 1 ? " (player " + player + ")" : ""));
            foreach (var row in Rows)
            {
                foreach (var box in new[] { row.Key, row.AltKey })
                {
                    if (box.Enabled && box.SelectedIndex > 0) text.AppendLine(row.Output + keyboardTarget + " = " + box.SelectedItem);
                }
            }
            text.AppendLine();
            text.AppendLine("# Controller bindings (every controller)");
            foreach (var row in Rows)
            {
                if (row.Pad.Enabled && row.Pad.SelectedIndex > 0) text.AppendLine(row.Output + " = " + row.Pad.SelectedItem);
            }
            text.AppendLine();
            text.AppendLine("# Range of deadzones: 1 (almost none) to 127 (max), for players 1-2");
            for (int p = 1; p <= 4; ++p)
            {
                string target = p > 1 ? ":" + p : "";
                text.AppendLine("analog_deadzone" + target + " = leftjoystick, " + leftDeadzone.Value + ", 127");
                text.AppendLine("analog_deadzone" + target + " = rightjoystick, " + rightDeadzone.Value + ", 127");
            }
            if (File.Exists(path))
            {
                var kept = new List<string>();
                foreach (var line in File.ReadAllLines(path))
                {
                    string output, input;
                    if (Parse(line, out output, out input) && !IsManaged(output)) kept.Add(line.Trim());
                }
                if (kept.Count > 0)
                {
                    text.AppendLine();
                    text.AppendLine("# Other bindings");
                    foreach (var line in kept) text.AppendLine(line);
                }
            }
            else
            {
                text.AppendLine();
                text.AppendLine("override_controller_color = false, 0, 0, 255");
            }
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            if (File.Exists(path)) File.Copy(path, path + ".bak", true);
            File.WriteAllText(path, text.ToString(), new UTF8Encoding(false));
            saveKeyboardPlayer(player);
        }
    }
}
