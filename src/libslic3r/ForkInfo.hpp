#ifndef slic3r_ForkInfo_hpp_
#define slic3r_ForkInfo_hpp_

#include <string>
#include <vector>

namespace Slic3r {

// One topic branch merged into this build (doc/fork/features/<id>/README.md frontmatter).
struct ForkFeature
{
    std::string id;
    std::string kind;        // feature | fix | tooling
    std::string title;
    std::string upstream_pr; // none | <url> | merged:<version_X>
    bool        show;        // listed in the About dialog
};

// Build identity of this fork build, generated at build time from git
// (cmake/modules/ForkGitVersion.cmake). SLIC3R_VERSION stays the upstream version.
// Holds no commit hash / dirty flag on purpose: ordinary commits must not relink the app.
struct ForkInfo
{
    std::string label;           // fork label in tags, e.g. "ext"
    std::string id;              // release / rc tag, or "v2.9.6+ext-dev" for development builds
    std::string version;         // our semver of a release / rc, e.g. "1.1.0", else empty
    std::string tag;             // release / rc tag at HEAD (clean checkout), else empty
    std::string last_tag;        // last fork tag in the history of HEAD, empty if none
    std::string branch;
    std::string upstream_tag;    // e.g. "version_2.9.6"
    std::string upstream_commit;
    bool        is_release;      // clean checkout exactly at a release tag
    bool        is_rc;           // clean checkout exactly at a release candidate tag
    std::vector<ForkFeature> features;

    // "Release v2.9.6+ext-v1.1.0" / "Release candidate ..." / "Development build on dev/2.9.6"
    std::string summary_line() const;
    // "Based on upstream version_2.9.6 (b028299c77)"
    std::string upstream_line() const;
    // Comma separated ids; only features with show == true unless all is set.
    std::string feature_ids(bool all = false) const;
};

const ForkInfo& fork_info();

} // namespace Slic3r

#endif // slic3r_ForkInfo_hpp_
