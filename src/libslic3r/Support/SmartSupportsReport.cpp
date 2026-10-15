// Smart support painting: report, print config contract, recommended changes, fingerprint (plan §3.11, §6.4,
// §6.8; WP2c0).
// WP0 stub: empty / 0 / no-op.

#include "SmartSupportsInternal.hpp"

namespace Slic3r::SmartSupports {

std::vector<ContractIssue> check_contract(const PrintObjectConfig &cfg, PlatePolicy policy, const Params *derived)
{
    return {};
}

std::vector<std::pair<std::string, std::string>> recommended_changes(const PrintObjectConfig &cfg, PlatePolicy policy, bool include_advisory)
{
    return {};
}

void apply_recommended(ModelConfig &object_config, const PrintObjectConfig &effective, PlatePolicy policy, bool include_advisory) {}

uint64_t config_fingerprint(const DynamicPrintConfig &effective, const ModelObject &live_mo, size_t instance, const UserParams &up)
{
    return 0;
}

std::vector<ReportEntry> build_report(const AnalysisInput &in, const Plan &plan, const std::vector<Need> &verify_residual,
                                      int report_more_out[2])
{
    report_more_out[0] = 0;
    report_more_out[1] = 0;
    return {};
}

} // namespace Slic3r::SmartSupports
