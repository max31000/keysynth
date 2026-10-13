using Keysynth.Core;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;

namespace Keysynth.Infrastructure;

public static class DependencyInjection
{
    public static IServiceCollection AddInfrastructure(this IServiceCollection services, IConfiguration configuration)
    {
        // Read lazily: WebApplicationFactory overrides configuration after registration.
        static string ConnStr(IServiceProvider sp) =>
            sp.GetRequiredService<IConfiguration>().GetConnectionString("DefaultConnection")
            ?? throw new InvalidOperationException("ConnectionStrings:DefaultConnection is not configured");

        services.AddSingleton<IClock, SystemClock>();
        services.AddScoped<IUserRepository>(sp => new MySqlUserRepository(ConnStr(sp)));
        services.AddScoped<IDeviceRepository>(sp => new MySqlDeviceRepository(ConnStr(sp)));
        services.AddScoped<IPresetRepository>(sp => new MySqlPresetRepository(ConnStr(sp)));
        services.AddScoped<ISongRepository>(sp => new MySqlSongRepository(ConnStr(sp)));
        services.AddSingleton(sp => new MigrationRunner(ConnStr(sp), sp.GetRequiredService<ILogger<MigrationRunner>>()));

        services.Configure<KeysynthOptions>(configuration.GetSection("Keysynth"));
        services.AddSingleton<ISampleIndexProvider, SampleIndexProvider>();
        return services;
    }
}
