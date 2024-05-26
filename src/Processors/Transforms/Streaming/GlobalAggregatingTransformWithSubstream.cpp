#include <Processors/Transforms/Streaming/GlobalAggregatingTransformWithSubstream.h>

#include <Processors/Transforms/Streaming/AggregatingHelper.h>
#include <Processors/Transforms/convertToChunk.h>

namespace DB
{
namespace ErrorCodes
{
extern const int NOT_IMPLEMENTED;
extern const int UNSUPPORTED;
extern const int RECOVER_CHECKPOINT_FAILED;
}

namespace Streaming
{
GlobalAggregatingTransformWithSubstream::GlobalAggregatingTransformWithSubstream(Block header, AggregatingTransformParamsPtr params_)
    : AggregatingTransformWithSubstream(
        std::move(header),
        std::move(params_),
        "GlobalAggregatingTransformWithSubstream",
        ProcessorID::GlobalAggregatingTransformWithSubstreamID)
{
    assert(params->params.group_by == Aggregator::Params::GroupBy::OTHER);
    if (params->emit_changelog && params->emit_version)
        throw Exception(ErrorCodes::UNSUPPORTED, "'emit_version()' is not supported in global aggregation emit changelog");
}

SubstreamContextPtr GlobalAggregatingTransformWithSubstream::getOrCreateSubstreamContext(const SubstreamID & id)
{
    auto substream_ctx = AggregatingTransformWithSubstream::getOrCreateSubstreamContext(id);
    if (params->emit_changelog && !substream_ctx->hasField())
    {
        bool retract_enabled = false;
        substream_ctx->setField(
            {retract_enabled,
             /// Field serializer
             [](const std::any & field, WriteBuffer & wb, VersionType version) {
                 assert(version >= V2);
                 DB::writeBinary(std::any_cast<bool>(field), wb);
             },
             /// Field deserializer
             [](std::any & field, ReadBuffer & rb, VersionType version) {
                 /// NOTE: Can not convert old impl to new impl V2
                 if (version < V2)
                     throw Exception(
                         ErrorCodes::RECOVER_CHECKPOINT_FAILED,
                         "Failed to recover aggregation checkpoint with retract aggregated states from an incompatible version '{}'",
                         version);

                 DB::readBinary(std::any_cast<bool &>(field), rb);
             }});
    }
    return substream_ctx;
}

std::pair<bool, bool>
GlobalAggregatingTransformWithSubstream::executeOrMergeColumns(Chunk & chunk, const SubstreamContextPtr & substream_ctx)
{
    if (params->emit_changelog)
    {
        assert(!params->only_merge && !no_more_keys);

        auto num_rows = chunk.getNumRows();
        if (retractEnabled(substream_ctx)) [[likely]]
            return params->aggregator.executeAndRetractOnBlock(
                chunk.detachColumns(), 0, num_rows, substream_ctx->variants, key_columns, aggregate_columns);
        else
            return params->aggregator.executeOnBlock(
                chunk.detachColumns(), 0, num_rows, substream_ctx->variants, key_columns, aggregate_columns);
    }
    else
        return AggregatingTransformWithSubstream::executeOrMergeColumns(chunk, substream_ctx);
}

/// Finalize what we have in memory and produce a finalized Block
/// and push the block to downstream pipe
void GlobalAggregatingTransformWithSubstream::finalize(const SubstreamContextPtr & substream_ctx, const ChunkContextPtr & chunk_ctx)
{
    assert(substream_ctx);

    auto finalized_watermark = chunk_ctx->getWatermark();
    SCOPE_EXIT({
        substream_ctx->resetRowCounts();
        substream_ctx->finalized_watermark = finalized_watermark;
    });

    /// If there is no new data, don't emit aggr result
    if (!substream_ctx->hasNewData())
        return;

    auto & variants = substream_ctx->variants;
    if (variants.empty())
        return;

    auto start = MonotonicMilliseconds::now();
    ChunkList chunks;
    if (params->emit_changelog)
    {
        chunks = AggregatingHelper::convertToChangelogChunks(variants, *params);
        /// Enable retract after first finalization
        retractEnabled(substream_ctx) |= !chunks.empty();
    }
    else
    {
        if (AggregatingHelper::onlyEmitUpdates(params->emit_mode))
            chunks = AggregatingHelper::convertUpdatesToChunks(variants, *params);
        else
            chunks = AggregatingHelper::convertToChunks(variants, *params);

        if (params->final && params->emit_version)
            emitVersion(chunks, substream_ctx);
    }

    if (chunks.empty()) [[unlikely]]
        chunks.emplace_back(getOutputs().front().getHeader().getColumns(), 0);

    /// Set chunk context for the last chunk
    chunks.back().setChunkContext(chunk_ctx);
    setAggregatedResult(chunks);

    auto end = MonotonicMilliseconds::now();

    LOG_INFO(log, "Took {} milliseconds to finalize aggregation", end - start);
}

bool & GlobalAggregatingTransformWithSubstream::retractEnabled(const SubstreamContextPtr & substream_ctx) const noexcept
{
    return substream_ctx->getField<bool &>();
}

}
}
