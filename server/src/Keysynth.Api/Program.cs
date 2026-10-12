using System.Text.Json;
using System.Text.Json.Serialization;
using Keysynth.Api.Auth;
using Keysynth.Api.Endpoints;
using Keysynth.Api.Http;
using Keysynth.Infrastructure;
using Microsoft.AspNetCore.Diagnostics;

// keysynth-api: REST contract docs/specs/2026-10-12-cloud-api.md. Public base /keysynth/api; nginx strips /keysynth.
var builder = WebApplication.CreateBuilder(args);

builder.Services.AddInfrastructure(builder.Configuration);
builder.Services.AddKeysynthAuth(builder.Configuration);
builder.Services.AddKeysynthRateLimits();

builder.Services.ConfigureHttpJsonOptions(o =>
{
    o.SerializerOptions.PropertyNamingPolicy = JsonNamingPolicy.CamelCase;
    o.SerializerOptions.Converters.Add(new JsonStringEnumConverter(JsonNamingPolicy.CamelCase));
});

// Production is same-origin (no CORS). Dev UI server only, and only in Development.
const string DevCors = "dev";
if (builder.Environment.IsDevelopment())
    builder.Services.AddCors(o => o.AddPolicy(DevCors, p => p
        .WithOrigins("http://localhost:5173").AllowAnyHeader().AllowAnyMethod().AllowCredentials()
        .WithExposedHeaders("ETag", "Retry-After")));

var app = builder.Build();

// Unhandled exceptions → 500 {"error":"internal"} (details only in the log).
app.UseExceptionHandler(e => e.Run(async ctx =>
{
    var ex = ctx.Features.Get<IExceptionHandlerFeature>()?.Error;
    var status = ex is BadHttpRequestException bad ? bad.StatusCode : StatusCodes.Status500InternalServerError;
    ctx.Response.StatusCode = status;
    await ctx.Response.WriteAsJsonAsync(new { error = Errors.ForStatus(status) });
}));
// Bodiless framework errors (unknown route, 405, …) get an {error} body too.
app.UseStatusCodePages(async sc =>
{
    var res = sc.HttpContext.Response;
    await res.WriteAsJsonAsync(new { error = Errors.ForStatus(res.StatusCode) });
});

if (app.Environment.IsDevelopment())
    app.UseCors(DevCors);

app.UseAuthentication();
app.UseAuthorization();
app.UseMiddleware<UserProvisioningMiddleware>();
app.UseRateLimiter();

// Migrations on startup (tests prepare their own storage).
if (!app.Environment.IsEnvironment("Testing"))
    await app.Services.GetRequiredService<MigrationRunner>().RunAsync();

app.MapGet("/health", () => Results.Ok(new { status = "ok" })).AllowAnonymous();
app.MapDeviceEndpoints();
app.MapSampleEndpoints();
app.MapPresetEndpoints();

app.Run();

/// <summary>Entry point type for WebApplicationFactory.</summary>
public partial class Program;
