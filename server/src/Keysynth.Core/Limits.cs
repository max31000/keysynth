namespace Keysynth.Core;

/// <summary>Limits of the REST contract (docs/specs/2026-10-12-cloud-api.md §7, §2.2, §2.3).</summary>
public static class Limits
{
    public const int MaxRequestBodyBytes = 1024 * 1024;
    public const int MaxPatchBytes = 256 * 1024;
    public const int MaxPresetsPerUser = 1000;
    public const int MaxDevicesPerUser = 10;
    public const int MaxDeviceNameLength = 100;
    public const int MaxUserNameLength = 255;

    public static readonly TimeSpan DeviceTokenLifetime = TimeSpan.FromDays(180);
    public static readonly TimeSpan AppOkMaxAge = TimeSpan.FromDays(30);
    public static readonly TimeSpan AppOkWriteInterval = TimeSpan.FromHours(1);
    public static readonly TimeSpan LastSeenWriteInterval = TimeSpan.FromMinutes(1);

    public const int DeviceMintPerHour = 10;
    public const int PresetWritesPerMinute = 120;
    public const int ManifestPerMinute = 60;
}
