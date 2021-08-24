// Copyright (c) 2012-2021 Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI
{
    /// <summary>
    /// The base class for <see cref="CurveBase{T}"/> editors. Allows to use generic curve editor without type information at compile-time.
    /// </summary>
    [HideInEditor]
    public abstract class CurveEditorBase : ContainerControl, IKeyframesEditor
    {
        /// <summary>
        /// The UI use mode flags.
        /// </summary>
        [Flags]
        public enum UseMode
        {
            /// <summary>
            /// Disable usage.
            /// </summary>
            Off = 0,

            /// <summary>
            /// Allow only vertical usage.
            /// </summary>
            Vertical = 1,

            /// <summary>
            /// Allow only horizontal usage.
            /// </summary>
            Horizontal = 2,

            /// <summary>
            /// Allow both vertical and horizontal usage.
            /// </summary>
            On = Vertical | Horizontal,
        }

        /// <summary>
        /// Occurs when curve gets edited.
        /// </summary>
        public event Action Edited;

        /// <summary>
        /// Occurs when curve data editing starts (via UI).
        /// </summary>
        public event Action EditingStart;

        /// <summary>
        /// Occurs when curve data editing ends (via UI).
        /// </summary>
        public event Action EditingEnd;

        /// <summary>
        /// The function for custom view panning. Gets input movement delta (in curve control space) and returns the renaming input delta to process by curve editor itself.
        /// </summary>
        public Func<Vector2, Vector2> CustomViewPanning;

        /// <summary>
        /// The maximum amount of keyframes to use in a single curve.
        /// </summary>
        public int MaxKeyframes = ushort.MaxValue;

        /// <summary>
        /// True if enable view zooming. Otherwise user won't be able to zoom in or out.
        /// </summary>
        public UseMode EnableZoom = UseMode.On;

        /// <summary>
        /// True if enable view panning. Otherwise user won't be able to move the view area.
        /// </summary>
        public UseMode EnablePanning = UseMode.On;

        /// <summary>
        /// Gets or sets the scroll bars usage.
        /// </summary>
        public abstract ScrollBars ScrollBars { get; set; }

        /// <summary>
        /// Enables drawing start/end values continuous lines.
        /// </summary>
        public bool ShowStartEndLines;

        /// <summary>
        /// Enables drawing background.
        /// </summary>
        public bool ShowBackground = true;

        /// <summary>
        /// Enables drawing time and values axes (lines and labels).
        /// </summary>
        public UseMode ShowAxes = UseMode.On;

        /// <summary>
        /// Gets the type of the curves keyframes value.
        /// </summary>
        public abstract Type ValueType { get; }

        /// <summary>
        /// The amount of frames per second of the curve animation (optional). Can be used to restrict the keyframes time values to the given time quantization rate.
        /// </summary>
        public abstract float? FPS { get; set; }

        /// <summary>
        /// Gets or sets a value indicating whether show curve collapsed as a list of keyframe points rather than a full curve.
        /// </summary>
        public abstract bool ShowCollapsed { get; set; }

        /// <summary>
        /// Gets or sets the view offset (via scroll bars).
        /// </summary>
        public abstract Vector2 ViewOffset { get; set; }

        /// <summary>
        /// Gets or sets the view scale.
        /// </summary>
        public abstract Vector2 ViewScale { get; set; }

        /// <summary>
        /// Gets the amount of keyframes added to the curve.
        /// </summary>
        public abstract int KeyframesCount { get; }

        /// <summary>
        /// Clears the selection.
        /// </summary>
        public abstract void ClearSelection();

        /// <summary>
        /// Called when curve gets edited.
        /// </summary>
        public void OnEdited()
        {
            Edited?.Invoke();
        }

        /// <summary>
        /// Called when curve data editing starts (via UI).
        /// </summary>
        public void OnEditingStart()
        {
            EditingStart?.Invoke();
        }

        /// <summary>
        /// Called when curve data editing ends (via UI).
        /// </summary>
        public void OnEditingEnd()
        {
            EditingEnd?.Invoke();
        }

        /// <summary>
        /// Updates the keyframes positioning.
        /// </summary>
        public abstract void UpdateKeyframes();

        /// <summary>
        /// Updates the tangents positioning.
        /// </summary>
        public abstract void UpdateTangents();

        /// <summary>
        /// Shows the whole curve.
        /// </summary>
        public abstract void ShowWholeCurve();

        /// <summary>
        /// Resets the view.
        /// </summary>
        public void ResetView()
        {
            ViewScale = ApplyUseModeMask(EnableZoom, Vector2.One, ViewScale);
            ViewOffset = ApplyUseModeMask(EnablePanning, Vector2.Zero, ViewOffset);
            UpdateKeyframes();
        }

        /// <summary>
        /// Evaluates the animation curve value at the specified time.
        /// </summary>
        /// <param name="result">The interpolated value from the curve at provided time.</param>
        /// <param name="time">The time to evaluate the curve at.</param>
        /// <param name="loop">If true the curve will loop when it goes past the end or beginning. Otherwise the curve value will be clamped.</param>
        public abstract void Evaluate(out object result, float time, bool loop = false);

        /// <summary>
        /// Gets the keyframes collection (as boxed objects).
        /// </summary>
        /// <returns>The array of boxed keyframe values of type <see cref="BezierCurve{T}.Keyframe"/> or <see cref="LinearCurve{T}.Keyframe"/>.</returns>
        public abstract object[] GetKeyframes();

        /// <summary>
        /// Sets the keyframes collection (as boxed objects).
        /// </summary>
        /// <param name="keyframes">The array of boxed keyframe values of type <see cref="BezierCurve{T}.Keyframe"/> or <see cref="LinearCurve{T}.Keyframe"/>.</param>
        public abstract void SetKeyframes(object[] keyframes);

        /// <summary>
        /// Adds the new keyframe (as boxed object).
        /// </summary>
        /// <param name="time">The keyframe time.</param>
        /// <param name="value">The keyframe value.</param>
        public abstract void AddKeyframe(float time, object value);

        /// <summary>
        /// Gets the keyframe data (as boxed objects).
        /// </summary>
        /// <param name="index">The keyframe index.</param>
        /// <param name="time">The keyframe time.</param>
        /// <param name="value">The keyframe value (boxed).</param>
        /// <param name="tangentIn">The keyframe 'In' tangent value (boxed).</param>
        /// <param name="tangentOut">The keyframe 'Out' tangent value (boxed).</param>
        public abstract void GetKeyframe(int index, out float time, out object value, out object tangentIn, out object tangentOut);

        /// <summary>
        /// Gets the existing keyframe value (as boxed object).
        /// </summary>
        /// <param name="index">The keyframe index.</param>
        /// <returns>The keyframe value.</returns>
        public abstract object GetKeyframe(int index);

        /// <summary>
        /// Sets the existing keyframe value (as boxed object).
        /// </summary>
        /// <param name="index">The keyframe index.</param>
        /// <param name="value">The keyframe value.</param>
        public abstract void SetKeyframeValue(int index, object value);

        /// <summary>
        /// Gets the keyframe point (in keyframes space).
        /// </summary>
        /// <param name="index">The keyframe index.</param>
        /// <param name="component">The keyframe value component index.</param>
        /// <returns>The point in time/value space.</returns>
        public abstract Vector2 GetKeyframePoint(int index, int component);

        /// <summary>
        /// Converts the <see cref="UseMode"/> into the <see cref="Vector2"/> mask.
        /// </summary>
        /// <param name="mode">The mode.</param>
        /// <returns>The mask.</returns>
        protected static Vector2 GetUseModeMask(UseMode mode)
        {
            return new Vector2((mode & UseMode.Horizontal) == UseMode.Horizontal ? 1.0f : 0.0f, (mode & UseMode.Vertical) == UseMode.Vertical ? 1.0f : 0.0f);
        }

        /// <summary>
        /// Filters the given value using the <see cref="UseMode"/>.
        /// </summary>
        /// <param name="mode">The mode.</param>
        /// <param name="value">The value to process.</param>
        /// <param name="defaultValue">The default value.</param>
        /// <returns>The combined value.</returns>
        protected static Vector2 ApplyUseModeMask(UseMode mode, Vector2 value, Vector2 defaultValue)
        {
            return new Vector2(
                               (mode & UseMode.Horizontal) == UseMode.Horizontal ? value.X : defaultValue.X,
                               (mode & UseMode.Vertical) == UseMode.Vertical ? value.Y : defaultValue.Y
                              );
        }

        /// <inheritdoc />
        public IKeyframesEditorContext KeyframesEditorContext { get; set; }

        /// <inheritdoc />
        public abstract void OnKeyframesDeselect(IKeyframesEditor editor);

        /// <inheritdoc />
        public abstract void OnKeyframesSelection(IKeyframesEditor editor, ContainerControl control, Rectangle selection);

        /// <inheritdoc />
        public abstract int OnKeyframesSelectionCount();

        /// <inheritdoc />
        public abstract void OnKeyframesDelete(IKeyframesEditor editor);
    }
}
