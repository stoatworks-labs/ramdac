/*
 * Stoatworks Labs - About window data for Ramdac.
 *
 * A PROVISIONAL hand copy, adapted from slitscan's generated one on
 * 2026-11-02, because Ramdac is not registered in stoatworks-backend yet. When
 * it is, scripts/sync-about.py generates this file and overwrites it. `guide` is
 * empty because no user guide exists yet, so the About block has no User guide
 * button (StoatworksAboutLinks.h leaves a missing link out), as graticule's
 * first build did.
 *
 * `version` here is a fallback. Anything with a build step injects the real one
 * at build time and overrides this.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "Ramdac";
    inline constexpr auto slug = "ramdac";
    inline constexpr auto hook = "Indexed colour: a palette, a swap and colour cycling, for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "";
    inline constexpr auto page = "https://stoatworks-labs.com/software/ramdac/";
    inline constexpr auto repo = "https://github.com/stoatworks-labs/ramdac";
    inline constexpr auto versionFallback = "v0.1.0";

    inline constexpr auto org = "Stoatworks Labs";
    inline constexpr auto home = "https://stoatworks-labs.com";
    inline constexpr auto tagline = "Open tools for the people who run the show.";

    /* The canonical funding links, matching FUNDING.yml and the support footer. */
    struct Link { const char* name; const char* url; };
    inline constexpr Link funding[] = {
        { "GitHub Sponsors", "https://github.com/sponsors/stoatworks-labs" },
        { "Ko-fi", "https://ko-fi.com/stoatworkslabs" },
        { "Patreon", "https://patreon.com/StoatworksLabs" },
        { "Liberapay", "https://liberapay.com/stoatworks-labs" },
    };
}
