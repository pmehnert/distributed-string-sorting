// (c) 2023 Pascal Mehnert
// This code is licensed under BSD 2-Clause License (see LICENSE for details)

#pragma once

#include <algorithm>
#include <numeric>

#include <tlx/sort/strings/radix_sort.hpp>

#include "mpi/communicator.hpp"
#include "sorter/distributed/prefix_doubling.hpp"
#include "strings/stringcontainer.hpp"
#include "strings/stringset.hpp"
#include "tlx/math/div_ceil.hpp"
#include "util/measuringTool.hpp"

namespace dss_mehnert {
namespace sorter {

template <
    typename Char,
    typename Subcommunicators,
    typename SamplePolicy,
    typename PartitionPolicy,
    typename SorterPolicy>
class SpaceEfficientSort : private SamplePolicy {
public:
    using StringSet = CompressedStringSet<Char, StringIndex, PEIndex>;
    using StringPtr = tlx::sort_strings_detail::StringLcpPtr<StringSet, size_t>;
    using String = StringSet::String;

    using MaterializedStringSet = dss_mehnert::StringSet<Char, Length, StringIndex, PEIndex>;

    SpaceEfficientSort(SamplePolicy const sampler, size_t const quantile_size)
        : SamplePolicy{sampler},
          sorter_{sampler},
          quantile_size_{std::max<size_t>(1, quantile_size)} {}

    std::vector<StringIndexPEIndex>
    sort(StringLcpContainer<CompressedStringSet<Char>>&& container, Subcommunicators const& comms) {
        this->measuring_tool_.start("init_container");
        // create a new container with additional rank and string index data members
        std::vector<typename StringSet::String> strings;
        strings.reserve(container.size());

        auto const rank = comms.comm_root().rank();
        for (size_t index = 0; auto const& src: container.get_strings()) {
            strings.push_back(src.with_members(StringIndex{index++}, PEIndex{rank}));
        }

        StringLcpContainer<StringSet> index_container{
            container.release_raw_strings(),
            std::move(strings),
            container.release_lcps()};
        container.delete_all();
        this->measuring_tool_.stop("init_container");

        return sort(std::move(index_container), comms);
    }

    std::vector<StringIndexPEIndex>
    sort(StringLcpContainer<StringSet>&& container, Subcommunicators const& comms) {
        auto const strptr = container.make_string_lcp_ptr();
        auto const& comm_root = comms.comm_root();

        this->measuring_tool_.setPhase("local_sorting");

        // todo debug total size of strings
        this->measuring_tool_.add(container.char_size(), "chars_in_set");

        measuring_tool_.start("local_sorting", "sort_locally");
        tlx::sort_strings_detail::radixsort_CI3(strptr, 0, 0);
        measuring_tool_.stop("local_sorting", "sort_locally", comms.comm_root());

        if (comm_root.size() == 1) {
            return write_permutation(strptr.active());
        }

        measuring_tool_.start("compute_quantiles", "compute_quantiles");
        auto const [quantile_sizes, quantile_offsets] =
            compute_quantiles(strptr, comms.comm_root());
        measuring_tool_.stop("compute_quantiles", "compute_quantiles");

        // todo alternative, put into struct
        // std::vector<size_t> ranks, indices;
        std::vector<StringIndexPEIndex> result;

        // SorterPolicy sorter{static_cast<SamplePolicy const&>(*this)};

        for (size_t i = 0; i < quantile_sizes.size(); ++i) {
            measuring_tool_.setQuantile(i);
            auto const quantile = strptr.sub(quantile_offsets[i], quantile_sizes[i]);

            // todo auto const total_local_size = strptr.active().get_sum_length();

            // todo the usage of StringContainer is slightly dodgy here
            // the strings are only actually materialized later
            StringLcpContainer<MaterializedStringSet> materialized_strings{
                std::vector<Char>{},
                std::vector<String>{quantile.active().begin(), quantile.active().end()},
                std::vector<size_t>{quantile.lcp(), quantile.lcp() + quantile.size()}};

            auto permutation = sorter_.sort(std::move(materialized_strings), comms, true);
            std::copy(permutation.begin(), permutation.end(), std::back_inserter(result));
        }

        measuring_tool_.setQuantile(0);
        return result;
    }

private:
    using MeasuringTool = measurement::MeasuringTool;
    MeasuringTool& measuring_tool_ = MeasuringTool::measuringTool();

    // private inheritance would cause problems here because SorterPolicy may
    // also inherit from SamplePolicy
    SorterPolicy sorter_;

    size_t quantile_size_;

    std::pair<std::vector<size_t>, std::vector<size_t>>
    compute_quantiles(StringPtr const& strptr, Communicator const& comm) {
        auto const total_size = strptr.active().get_sum_length();
        size_t const num_quantiles = tlx::div_ceil(total_size, quantile_size_);

        if (num_quantiles == 1) {
            // todo get rid of this bodge
            return {{strptr.size()}, {0}};
        }

        measuring_tool_.start("sample_quantiles");
        auto sample = SamplePolicy::sample_splitters(
            strptr.active(),
            num_quantiles,
            sample::NoExtraArg{},
            comm
        );
        measuring_tool_.stop("sample_quantiles");


        measuring_tool_.start("sort_quantiles");
        auto sizes =
            PartitionPolicy::compute_partition(strptr, std::move(sample), num_quantiles, comm);

        std::vector<size_t> offsets(sizes.size());
        std::exclusive_scan(sizes.begin(), sizes.end(), offsets.begin(), size_t{0});
        measuring_tool_.stop("sort_quantiles");

        return {std::move(sizes), std::move(offsets)};
    }

    // todo get rid of this copy paste
    std::vector<StringIndexPEIndex> write_permutation(StringSet const& ss) {
        this->measuring_tool_.start("writeback_permutation");
        std::vector<StringIndexPEIndex> permutation(ss.size());

        std::transform(ss.begin(), ss.end(), permutation.begin(), [&ss](auto const& str) {
            return StringIndexPEIndex{str.getStringIndex(), str.getPEIndex()};
        });
        this->measuring_tool_.stop("writeback_permutation");

        this->measuring_tool_.setPhase("none");
        return permutation;
    }
};

} // namespace sorter
} // namespace dss_mehnert