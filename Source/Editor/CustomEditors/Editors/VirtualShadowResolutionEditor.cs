// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEditor.GUI.Input;
using FlaxEngine;

namespace FlaxEditor.CustomEditors.Editors
{
    /// <summary>
    /// Edits dense world shadow grid resolution in power-of-two steps.
    /// </summary>
    public sealed class VirtualShadowResolutionEditor : CustomEditor
    {
        private const int MinExponent = 9;
        private const int MaxExponent = 11;

        private ResolutionSlider _slider;

        /// <inheritdoc />
        public override DisplayStyle Style => DisplayStyle.Inline;

        /// <inheritdoc />
        public override void Initialize(LayoutElementsContainer layout)
        {
            _slider = layout.Custom<ResolutionSlider>().CustomControl;
            _slider.ValueChanged += OnValueChanged;
            _slider.SlidingEnd += ClearToken;
        }

        /// <inheritdoc />
        public override void Refresh()
        {
            base.Refresh();
            if (HasDifferentValues || _slider.IsSliding)
                return;
            int resolution = (int)Values[0];
            int exponent = MinExponent;
            while (exponent < MaxExponent && resolution >= (1 << (exponent + 1)))
                exponent++;
            _slider.Value = exponent;
        }

        private void OnValueChanged()
        {
            int resolution = 1 << Mathf.RoundToInt(_slider.Value);
            if (HasDifferentValues || (int)Values[0] != resolution)
                SetValue(resolution, _slider.IsSliding ? this : null);
        }

        private sealed class ResolutionSlider : SliderControl
        {
            public ResolutionSlider()
                : base(MinExponent, min: MinExponent, max: MaxExponent)
            {
                _textBox.IsReadOnly = true;
                UpdateText();
            }

            protected override void UpdateText()
            {
                int resolution = 1 << Mathf.RoundToInt(Value);
                _textBox.Text = resolution < 1024 ? resolution.ToString() : (resolution / 1024) + "K";
            }
        }
    }
}
