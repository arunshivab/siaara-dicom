// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

namespace Siaara.Viewer.Core.Measure.Suv;

/// <summary>Body size measures for SUV normalisation (weight in kg, height in cm).</summary>
public static class BodySize
{
    /// <summary>Lean body mass, James (1976), kg. Not valid at high BMI (can become negative).</summary>
    /// <param name="weightKg">Weight.</param>
    /// <param name="heightCm">Height.</param>
    /// <param name="male">Male formula.</param>
    /// <returns>kg.</returns>
    public static double LeanBodyMassJames(double weightKg, double heightCm, bool male)
    {
        double r = weightKg / heightCm;
        return male ? (1.10 * weightKg) - (128.0 * r * r) : (1.07 * weightKg) - (148.0 * r * r);
    }

    /// <summary>Lean body mass, Janmahasatian et al. (2005), kg.</summary>
    /// <param name="weightKg">Weight.</param>
    /// <param name="heightCm">Height.</param>
    /// <param name="male">Male formula.</param>
    /// <returns>kg.</returns>
    public static double LeanBodyMassJanmahasatian(double weightKg, double heightCm, bool male)
    {
        double hm = heightCm / 100.0;
        double bmi = weightKg / (hm * hm);
        return male ? 9270.0 * weightKg / (6680.0 + (216.0 * bmi)) : 9270.0 * weightKg / (8780.0 + (244.0 * bmi));
    }

    /// <summary>Body surface area, Du Bois and Du Bois (1916), m².</summary>
    /// <param name="weightKg">Weight.</param>
    /// <param name="heightCm">Height.</param>
    /// <returns>m².</returns>
    public static double SurfaceAreaDuBois(double weightKg, double heightCm) => 0.007184 * Math.Pow(weightKg, 0.425) * Math.Pow(heightCm, 0.725);

    /// <summary>Ideal body weight, Devine (1974), kg.</summary>
    /// <param name="heightCm">Height.</param>
    /// <param name="male">Male formula.</param>
    /// <returns>kg.</returns>
    public static double IdealBodyWeightDevine(double heightCm, bool male)
    {
        double inches = heightCm / 2.54;
        return (male ? 50.0 : 45.5) + (2.3 * (inches - 60.0));
    }
}
