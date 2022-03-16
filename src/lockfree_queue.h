#pragma once
#include <atomic>
#include <cstdint>
#include <cassert>

namespace ksgw {

    constexpr std::memory_order load_order = std::memory_order::memory_order_relaxed;
    constexpr std::memory_order write_order = std::memory_order::memory_order_relaxed;
    namespace internal {

        constexpr uint32_t CacheLineSize = 64;

        template<class T>
        class Ptr {
            union {
                struct {
                    uint64_t addr: 48;
                    uint64_t vers: 16;
                };
                uint64_t _raw;
            };
        public:
            Ptr() : _raw(0) {}
            Ptr(T* ptr, uint64_t ver):
                addr(reinterpret_cast<uint64_t>(ptr)),
                vers(ver)
            {}
            //
            T* operator->() {
                return reinterpret_cast<T*>(addr);
            }
            uint64_t ver() const {
                return vers;
            }
            void upgrade() {
                vers++;
            }
            void setPtr(T* ptr) {
                addr = reinterpret_cast<uint64_t>(ptr);
            }
            void setVer(uint64_t ver) {
                vers = ver;
            }
            T* ptr() {
                return reinterpret_cast<T*>(addr);
            }
            bool addrEqual( const Ptr<T>& other ) const {
                return addr == other.addr;
            }
            bool operator == (const Ptr& other) const {
                return _raw == other._raw;
            }
            bool isNull() const {
                return addr == 0;
            }
            const uint64_t& raw() const {
                return _raw;
            }
        };

        template<class T>
        class alignas(8) Node {
        private:
            static_assert(sizeof(Ptr<Node<T>>) == sizeof(uint64_t), "Ptr<Node<T>> must be 8 bytes" );
            //
            std::atomic<Ptr<Node>>  _next;
            std::atomic<T>          _data;
            /**
             * @brief 
             * 在队列的pop函数里我们在更新_head之后，就可以获取next的数据了，然而有可能其它线程又立即获取了现在的next作为他们线程状态里的_head，
             * 然后析构掉，如果这个操作是在我们在当前线程获取值之前，那么这个逻辑就会出现错误，因此，我们需要加一个引用计数，
             * 默认为2，获取值释放一次，pop head节点时释放一次
             */
            std::atomic<size_t>     _ref;
        public:
            Node(T&& t) 
                : _next(Ptr<Node>(nullptr, 0))
                , _data(std::move(t))
                , _ref(1){
            }
            std::atomic<Ptr<Node>>& next() {
                return _next;
            }
            T data() {
                return _data.load(load_order);
            }
            void release() {
                size_t ref = _ref.fetch_sub(1, load_order);
                assert(ref < 2);
                if(!ref) {
                    delete this;
                }
            }
        };
    }

    namespace list_based {

        template<class T>
        using Ptr = internal::Ptr<T>;
        template<class T>
        using Node = internal::Node<T>;

        template<class T>
        class Queue {
        private:
            alignas(internal::CacheLineSize) std::atomic<Ptr<Node<T>>> _head;
            alignas(internal::CacheLineSize) std::atomic<Ptr<Node<T>>> _tail;
        public:
            Queue() {
                Node<T>* node = new Node<T>(T());
                node->release();
                Ptr<Node<T>> headPtr(node, 0);
                Ptr<Node<T>> tailPtr(node, 0x7fff);
                _head.store(headPtr);
                _tail.store(tailPtr);
            }

            ~Queue() {
                while(true) {
                    T t;
                    if(!(pop(t) == 0)) {
                        break;
                    }
                }
                _head.load(load_order).ptr()->release();
            }

            void push(T&& t) {
                Ptr<Node<T>> node_ptr(new Node<T>(std::move(t)), 0);
                while(true) {
                    Ptr<Node<T>> tail = _tail.load(load_order);
                    Ptr<Node<T>> next = tail.ptr()->next().load(load_order);
                    Ptr<Node<T>> tail2 = _tail.load(load_order);
                    if(!tail.addrEqual(tail2)) {
                        continue;
                    }
                    if(!next.isNull()) {
                        _tail.compare_exchange_strong(tail, next, write_order);
                        continue;
                    }
                    node_ptr.setVer(next.ver() + 1);
                    node_ptr.ptr()->next().store(Ptr<Node<T>>(nullptr, next.ver()+1), write_order);
                    if(!tail.ptr()->next().compare_exchange_strong(next, node_ptr, write_order)) {
                        continue;
                    }
                    auto rst = _tail.compare_exchange_strong(tail, node_ptr, write_order);
                    // 实际上，这里的rst是可能会失败的，因为上边next非空也可能会改next值
                    // assert(rst);
                    break;
                }
            }

            int pop(T& t) {
                while(true) {
                    Ptr<Node<T>> head = _head.load(load_order);
                    Ptr<Node<T>> tail = _tail.load(load_order);
                    Node<T>* head_ptr = head.ptr();
                    Ptr<Node<T>> next = head_ptr->next().load(load_order);
                    auto head2 = _head.load(load_order);
                    if(!head.addrEqual(head2)) {
                        continue;
                    }
                    if(head.addrEqual(tail) || next.isNull()) {
                        return -1;
                    }
                    next.upgrade();
                    if(_head.compare_exchange_strong(head, next, write_order)) {
                        Node<T>* next_ptr = next.ptr();
                        t = next_ptr->data();
                        next_ptr->release();
                        head_ptr->release();
                        return 0;
                    }
                }
            }
        };
    }

    
}