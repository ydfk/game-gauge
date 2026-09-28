#include "frame_statistics.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace gauge {
double slow_tail_fps(const std::vector<double>& frames, double fraction) {
    if (frames.empty() || fraction <= 0 || fraction > 1) return 0;
    auto sorted = frames;
    const auto count = std::max(size_t{1}, static_cast<size_t>(std::ceil(sorted.size() * fraction)));
    // 只需要最慢区间的总和，不必对全部帧排序。
    if (count < sorted.size()) std::nth_element(sorted.begin(), sorted.begin() + count, sorted.end(), std::greater<>());
    const double sum = std::accumulate(sorted.begin(), sorted.begin() + count, 0.0);
    return sum > 0 ? 1000.0 * count / sum : 0;
}
void FrameStatistics::add(uint64_t timestamp, uint64_t chain, double ms) {
    if (!std::isfinite(ms) || ms <= 0 || timestamp == 0) return;
    frames_.push_back({timestamp, chain, ms});
    // 时间窗口之外还设容量上限，避免高帧率游戏无限占用内存。
    while (!frames_.empty() && (frames_.size() > 120000 ||
        (timestamp >= frames_.front().timestamp_ms && timestamp - frames_.front().timestamp_ms > 60000))) frames_.pop_front();
}
void FrameStatistics::reset() { frames_.clear(); selected_chain_ = 0; }
void FrameStatistics::publish(Snapshot& s, uint64_t now) {
    while (!frames_.empty() && now >= frames_.front().timestamp_ms && now - frames_.front().timestamp_ms > 60000) frames_.pop_front();
    std::unordered_map<uint64_t, size_t> counts;
    for (const auto& frame : frames_) if (now >= frame.timestamp_ms && now - frame.timestamp_ms <= 1000) ++counts[frame.chain];
    if (counts.empty()) {
        // PresentMon 偶尔延迟交付一批帧；短暂空窗沿用最近一次读数，超过两秒再标记等待。
        if (!frames_.empty() && now >= frames_.back().timestamp_ms &&
            now - frames_.back().timestamp_ms <= 2000 && s.fps.state == State::valid) return;
        s.fps = missing(State::waiting, "等待目标游戏的帧事件", "PresentMon");
        s.frametime = s.fps;
        s.low1 = s.low01 = missing(State::waiting, "样本不足", "GameGauge 慢帧均值");
        s.frame_samples = 0;
        s.recent_frames.clear();
        return;
    }
    auto largest = std::max_element(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    if (!counts.contains(selected_chain_) || largest->second > counts[selected_chain_] * 2) selected_chain_ = largest->first;
    std::vector<double> values, recent;
    for (const auto& frame : frames_) if (frame.chain == selected_chain_ && frame.timestamp_ms <= now) {
        values.push_back(frame.milliseconds);
        if (now - frame.timestamp_ms <= 1000) recent.push_back(frame.milliseconds);
    }
    double sum = std::accumulate(recent.begin(), recent.end(), 0.0);
    s.fps = available(sum > 0 ? 1000.0 * recent.size() / sum : 0, "PresentMon · 呈现事件");
    s.frametime = available(sum / std::max(size_t{1}, recent.size()), "PresentMon · 呈现间隔");
    s.low1 = values.size() >= 1000 ? available(slow_tail_fps(values, .01), "60秒 · 最慢1%均值") : missing(State::waiting, "需要1000个有效间隔");
    s.low01 = values.size() >= 10000 ? available(slow_tail_fps(values, .001), "60秒 · 最慢0.1%均值") : missing(State::waiting, "需要10000个有效间隔");
    s.frame_samples = values.size();
    const auto start = values.size() > 180 ? values.size() - 180 : 0;
    s.recent_frames.assign(values.begin() + start, values.end());
}
}
