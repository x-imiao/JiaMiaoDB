/* ═══════════════════════════════════════════════════════════════════════
   skiplist.h — Phase 2 LSM: 真并发 SkipList (Pugh 风格)

   单写多读 (Single Writer / Multiple Reader, SWMR) lock-free 跳表.
   替换 Phase 1 的 std::map + shared_mutex shim.

   算法参考: William Pugh, "Skip Lists: A Probabilistic Alternative to
   Balanced Trees" (1990), 以及 Doug Lea 的 java.util.concurrent.ConcurrentSkipListMap
   的简化版本 (无删除 GC, 节点不 free, 走 arena 析构统一回收).

   关键设计:
   - 写路径: 拿 writer_mutex_ → 找每层前驱 → 安装节点 (atomic release).
   - 读路径: 纯 lock-free, atomic<Node*> acquire 读, 与 writer 的 release 配对.
   - 节点不 free: 避免 UAF; arena reset/析构时统一回收.
   - 内存序: acquire/release pair, 不需要 seq_cst 或 fence.

   Phase 2 不实现:
   - 单点 erase(K): 改用 erase_range(lo, hi) 给 MemTable::erase_all_for 用.
   - 读 size(): 非 atomic, 写时维护 size_, 读时不持锁, 值是 "approximate".
   - 迭代器失效: 写后 next 指针变化, 正在遍历的 reader 可能跳过新节点,
                 但已经遍历到的节点保证有效 (节点不 free).
   ═══════════════════════════════════════════════════════════════════════ */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "arena.h"

namespace jiamiao {

template <typename K, typename V>
class SkipList {
public:
    static constexpr int    kMaxLevel    = 20;        // 2^20 = 1M 元素, 足够
    static constexpr double kProbability = 0.5;       // P(升层) = 0.5

    explicit SkipList(Arena* arena)
        : arena_(arena) {
        // Header 是 sentinel: kMaxLevel+1 个 next[0..kMaxLevel], 全置 null.
        // 用 new (heap) 分配, 让 unique_ptr<Node> 正常析构 (key/value 默认构造).
        // 数据节点用 arena 分配, 走 arena reset/析构统一回收.
        size_t header_size = sizeof(Node) + (kMaxLevel + 1) * sizeof(std::atomic<Node*>);
        Node* hdr = static_cast<Node*>(::operator new(header_size, std::align_val_t{64}));
        hdr->key   = K{};
        hdr->value = V{};
        hdr->top_level = kMaxLevel;
        for (int i = 0; i <= kMaxLevel; ++i) {
            hdr->next[i].store(nullptr, std::memory_order_relaxed);
        }
        header_.reset(hdr);
        // 初始 level_ = 0 (空表)
        level_.store(0, std::memory_order_relaxed);
        size_.store(0, std::memory_order_relaxed);
    }

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    ~SkipList() {
        // 对齐释放 header (与 ::operator new(size, align_val_t{64}) 配对)
        if (header_) {
            Node* hdr = header_.release();
            hdr->~Node();  // 显式调析构 (K / V)
            ::operator delete(hdr, std::align_val_t{64});
        }
    }

    // ── 写 ──
    void put(const K& key, V value) {
        std::lock_guard<std::mutex> lk(writer_mutex_);

        Node* update[kMaxLevel + 1];
        Node* cur = header_.get();

        // 从最高层向下找每层前驱
        int cur_level = level_.load(std::memory_order_relaxed);
        for (int i = cur_level; i >= 0; --i) {
            Node* next = cur->next[i].load(std::memory_order_acquire);
            while (next != nullptr && next->key < key) {
                cur = next;
                next = cur->next[i].load(std::memory_order_acquire);
            }
            update[i] = cur;
        }

        // 第 0 层下一步就是要找的节点
        Node* found = cur->next[0].load(std::memory_order_acquire);

        if (found != nullptr && found->key == key) {
            // 已存在: 就地更新 value
            // 不改 top_level (层次结构不变)
            found->value = std::move(value);
            return;
        }

        // 新节点: 算层数
        int new_level = random_level();
        if (new_level > cur_level) {
            for (int i = cur_level + 1; i <= new_level; ++i) {
                update[i] = header_.get();
            }
            level_.store(new_level, std::memory_order_relaxed);
        }

        // 从 arena 分配 Node (含 flexible array)
        size_t node_size = sizeof(Node) + (new_level + 1) * sizeof(std::atomic<Node*>);
        Node* new_node = new (arena_->allocate_aligned(node_size, 64)) Node(new_level);
        new_node->key   = key;
        new_node->value = std::move(value);

        // 安装: 先连下家, 再连上家 (经典 Pugh 顺序)
        for (int i = 0; i <= new_level; ++i) {
            Node* expected = update[i]->next[i].load(std::memory_order_acquire);
            new_node->next[i].store(expected, std::memory_order_relaxed);
            update[i]->next[i].store(new_node, std::memory_order_release);
        }
        size_.fetch_add(1, std::memory_order_relaxed);
    }

    // ── 范围批量删 (给 MemTable::erase_all_for 用) ──
    //   物理摘除所有 key ∈ [lo, hi] 的节点. 节点不 free (arena 析构统一回收).
    //   返回摘除的节点数.
    //
    //   关键算法: 对每一层 i, 从 update[i] 开始向后扫, 跳过所有 key ∈ [lo, hi] 的节点,
    //   然后把 update[i]->next[i] 一步链到第一个 key > hi 的节点. 这样被删的节点直接
    //   被旁路, 不需要修改它们的 next[] (它们仍然指向"被删的链表"; reader 若已遍历到
    //   被删节点则继续顺着原链走到下一个 alive 节点, 这是 single-writer 模型下的可接受语义).
    size_t erase_range(const K& lo, const K& hi) {
        std::lock_guard<std::mutex> lk(writer_mutex_);

        Node* update[kMaxLevel + 1];
        Node* cur = header_.get();

        int cur_level = level_.load(std::memory_order_relaxed);
        for (int i = cur_level; i >= 0; --i) {
            Node* next = cur->next[i].load(std::memory_order_acquire);
            while (next != nullptr && next->key < lo) {
                cur = next;
                next = cur->next[i].load(std::memory_order_acquire);
            }
            update[i] = cur;
        }

        // 计数: 走 level 0 数被删节点 (level 0 含全部节点)
        size_t erased = 0;
        Node* node = cur->next[0].load(std::memory_order_acquire);
        while (node != nullptr && !(hi < node->key)) {
            ++erased;
            node = node->next[0].load(std::memory_order_acquire);
        }

        // 每层旁路: update[i]->next[i] 跳过 [lo, hi] 的所有节点
        for (int i = 0; i <= cur_level; ++i) {
            Node* nxt = update[i]->next[i].load(std::memory_order_acquire);
            while (nxt != nullptr && !(hi < nxt->key)) {
                nxt = nxt->next[i].load(std::memory_order_acquire);
            }
            update[i]->next[i].store(nxt, std::memory_order_release);
        }

        size_.fetch_sub(erased, std::memory_order_relaxed);
        return erased;
    }

    // ── 读 (lock-free) ──
    std::optional<V> get_exact(const K& key) const {
        Node* cur = header_->next[0].load(std::memory_order_acquire);
        while (cur != nullptr && cur->key < key) {
            cur = cur->next[0].load(std::memory_order_acquire);
        }
        if (cur != nullptr && cur->key == key) {
            return cur->value;  // 拷贝 (V 需可拷贝)
        }
        return std::nullopt;
    }

    std::vector<V> range(const K& lo, const K& hi) const {
        std::vector<V> out;
        Node* cur = header_->next[0].load(std::memory_order_acquire);
        // 跳到 >= lo
        while (cur != nullptr && cur->key < lo) {
            cur = cur->next[0].load(std::memory_order_acquire);
        }
        // 收 [lo, hi]
        while (cur != nullptr && !(hi < cur->key)) {
            out.push_back(cur->value);
            cur = cur->next[0].load(std::memory_order_acquire);
        }
        return out;
    }

    std::vector<V> all() const {
        std::vector<V> out;
        out.reserve(size_.load(std::memory_order_relaxed));
        Node* cur = header_->next[0].load(std::memory_order_acquire);
        while (cur != nullptr) {
            out.push_back(cur->value);
            cur = cur->next[0].load(std::memory_order_acquire);
        }
        return out;
    }

    // ── 状态 (非严格, 写时不持锁读) ──
    size_t size() const {
        return size_.load(std::memory_order_relaxed);
    }

    int level() const {
        return level_.load(std::memory_order_relaxed);
    }

private:
    // ── Node: 跳表节点 ──
    //   头节点: top_level = kMaxLevel, key 不使用.
    //   数据节点: top_level ∈ [0, kMaxLevel-1].
    struct Node {
        K key;
        V value;
        int top_level;                  // 节点最高层 (含), 范围 [0, kMaxLevel]
        std::atomic<Node*> next[];      // flexible array: next[0..top_level]

        explicit Node(int top) : top_level(top) {}
    };

    // ── 随机层数 (几何分布, P = 0.5) ──
    static int random_level() {
        thread_local std::mt19937 gen(
            static_cast<uint32_t>(
                std::hash<std::thread::id>()(std::this_thread::get_id())));
        thread_local std::geometric_distribution<int> dist(1.0 - kProbability);
        int lvl = 0;
        while (lvl < kMaxLevel - 1 && dist(gen) == 0) ++lvl;
        return lvl;
    }

    // Header 是普通指针 (new 出来), 内部 next[] 用 atomic 初始化.
    // 注: 不放 arena, 避免析构时与 arena 释放顺序死锁.
    std::unique_ptr<Node> header_;
    std::atomic<int>     level_;          // 当前表最高层
    std::atomic<size_t>  size_;           // 节点数 (approximate)
    Arena*               arena_;
    std::mutex           writer_mutex_;   // 仅 put / erase_range 用
};

}  // namespace jiamiao
