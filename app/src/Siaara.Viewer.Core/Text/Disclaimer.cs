// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

namespace Siaara.Viewer.Core.Text;

/// <summary>The intended-use label (M1.1 review, P0). One text everywhere: the About box, the patient
/// bar, and every exported or copied image, which carries the calculated values of its measurements.</summary>
public static class Disclaimer
{
    /// <summary>The label.</summary>
    public const string Text = "Not for primary diagnosis";

    /// <summary>Longer wording for the About box.</summary>
    public const string Details =
        "SIAARA Viewer is not cleared or approved as a medical device. Not for primary diagnosis: images and calculated values " +
        "(lengths, areas, ROI statistics, SUV) are for review and reference only.";
}
