#pragma once

#include <set>

#include <map>
#include <functional>
#include "Slic3r/Biz/Slicing/SlicingInteractor.hpp"
#include "Slic3r/Biz/Platform/WithListeners.hpp"
#include "Slic3r/Biz/libpgcode/ProcessorResult.hpp"

namespace Slic3r::Biz {

struct IFDMResultCacheChangedListener
{
    virtual ~IFDMResultCacheChangedListener() = default;
    virtual void on_fdm_result_cache_changed(const Domain::SlicingId id) = 0;
};

using FDMResultRef = std::reference_wrapper<const Slicing::FDMResult>;
class FDMResultCache :
    public Slicing::IFDMResultListener,
    public WithListeners<IFDMResultCacheChangedListener>
{
public:
    std::optional<FDMResultRef> get_result(const Domain::SlicingId id) const;

    /**
     * @brief G-code viewer: shows a result that does not come from slicing (a G-code file).
     * Empty results from slicing (the bed has nothing to slice) do not replace it; a real
     * slicing result does.
     */
    void set_pinned_result(Slicing::FDMResult&& result, const Domain::SlicingId id);

    void on_fdm_result_changed(
        Slicing::FDMResult&& result,
        const Domain::SlicingId id
    ) override;

private:
    std::map<Domain::SlicingId, Slicing::FDMResult> m_results;
    std::set<Domain::SlicingId> m_pinned;

    std::size_t memsize() const;
};
} // namespace Slic2r::Biz::FDMResultCache
