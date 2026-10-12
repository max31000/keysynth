# keysynth-api. Build context: server/ (docker build -f server/Dockerfile server).
FROM mcr.microsoft.com/dotnet/sdk:8.0 AS build
WORKDIR /src
COPY global.json ./
COPY src/Keysynth.Core/Keysynth.Core.csproj src/Keysynth.Core/
COPY src/Keysynth.Infrastructure/Keysynth.Infrastructure.csproj src/Keysynth.Infrastructure/
COPY src/Keysynth.Api/Keysynth.Api.csproj src/Keysynth.Api/
RUN dotnet restore src/Keysynth.Api/Keysynth.Api.csproj
COPY src/ src/
RUN dotnet publish src/Keysynth.Api/Keysynth.Api.csproj -c Release -o /app/publish --no-restore /p:UseAppHost=false

FROM mcr.microsoft.com/dotnet/aspnet:8.0 AS final
WORKDIR /app
ENV ASPNETCORE_HTTP_PORTS=8080 \
    ASPNETCORE_ENVIRONMENT=Production \
    Keysynth__SamplesRoot=/data/samples
EXPOSE 8080
COPY --from=build /app/publish .
# Non-root "app" user of the aspnet image (uid/gid 1654), platform convention.
USER 1654:1654
ENTRYPOINT ["dotnet", "Keysynth.Api.dll"]
