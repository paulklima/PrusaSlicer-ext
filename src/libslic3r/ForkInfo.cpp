#include "ForkInfo.hpp"

// Generated at build time into <build>/src/libslic3r/, only included here so that a new
// commit or tag recompiles this single file.
#include "fork_git_version.h"

namespace Slic3r {

const ForkInfo& fork_info()
{
    static const ForkInfo info {
        FORK_LABEL,
        FORK_ID,
        FORK_VERSION,
        FORK_TAG,
        FORK_LAST_TAG,
        FORK_BRANCH,
        FORK_UPSTREAM_TAG,
        FORK_UPSTREAM_COMMIT,
        FORK_IS_RELEASE != 0,
        FORK_IS_RC != 0,
        { FORK_FEATURES_INIT }
    };
    return info;
}

std::string ForkInfo::summary_line() const
{
    if (is_release)
        return "Release " + tag;
    if (is_rc)
        return "Release candidate " + tag;
    std::string out = "Development build on " + branch;
    if (!last_tag.empty())
        out += " (after " + last_tag + ")";
    return out;
}

std::string ForkInfo::upstream_line() const
{
    return "Based on upstream " + upstream_tag + " (" + upstream_commit + ")";
}

std::string ForkInfo::feature_ids(bool all) const
{
    std::string out;
    for (const ForkFeature& f : features)
        if (all || f.show)
            out += (out.empty() ? "" : ", ") + f.id;
    return out;
}

} // namespace Slic3r
