#pragma once
#include <atomic>
#include <cstdint>
#include <cassert>

namespace ksgw {

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
                return _data.load(std::memory_order::memory_order_relaxed);
            }
            void release() {
                size_t ref = _ref.fetch_sub(1, std::memory_order::memory_order_relaxed);
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
            alignas(CacheLineSize) std::atomic<Ptr<Node<T>>> _head;
            alignas(CacheLineSize) std::atomic<Ptr<Node<T>>> _tail;
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
                    if(!pop(t) == 0) {
                        break;
                    }
                }
                _head.load(std::memory_order::memory_order_relaxed).ptr()->release();
            }

            void push(T&& t) {
                Ptr<Node<T>> node_ptr(new Node<T>(std::move(t)), 0);
                while(true) {
                    Ptr<Node<T>> tail = _tail.load(std::memory_order::memory_order_relaxed);
                    auto& tail_next = tail.ptr()->next();
                    Ptr<Node<T>> next = tail_next.load(std::memory_order::memory_order_relaxed);
                    if(!next.isNull()) {
                        continue;
                    }
                    node_ptr.setVer(next.ver() + 1);
                    node_ptr.ptr()->next().store(Ptr<Node<T>>(nullptr, next.ver()+1), std::memory_order::memory_order_relaxed);
                    if(!tail_next.compare_exchange_weak(next, node_ptr, std::memory_order::memory_order_relaxed)) {
                        continue;
                    }
                    auto rst = _tail.compare_exchange_strong(tail, node_ptr, std::memory_order::memory_order_relaxed);
                    assert(rst);
                    break;
                }
            }

            int pop(T& t) {
                while(true) {
                    Ptr<Node<T>> head = _head.load(std::memory_order::memory_order_relaxed);
                    Ptr<Node<T>> tail = _tail.load(std::memory_order::memory_order_relaxed);
                    Node<T>* head_ptr = head.ptr();
                    Ptr<Node<T>> next = head_ptr->next().load(std::memory_order::memory_order_consume);
                    if(head.addrEqual(tail) || next.isNull()) {
                        return -1;
                    }
                    next.upgrade();
                    if(_head.compare_exchange_weak(head, next, std::memory_order::memory_order_release, std::memory_order::memory_order_relaxed)) {
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

    namespace array_based {

        template < class T >
        class Queue {
            static constexpr size_t InvalidPosition = ~0ULL;        //!> a constant value for reference
            static constexpr size_t CacheLineAlignmentSize = 64 - sizeof(size_t);
        private:
            struct Node {
                T                   val;
                std::atomic<size_t> readIndex;                      //!> readIndex is the value for get the status of read
                std::atomic<size_t> storeIndex;                     //!> storeIndex is the value of ( index + capacity*cycle )
            };

            Node*                   _nodeArray;                     //!> stores all nodes for the queue
            size_t                  _capacity;                      //!> the capacity of the queue
            size_t                  _capacityMask;                  //!> a utility value for location the index of the '_nodeArray'

            std::atomic<size_t>     _head;                          //!> head of the queue
            char                    _align[CacheLineAlignmentSize]; //!> cache line aligment, optimize for multi-core processors
            std::atomic<size_t>     _tail;                          //!> tail of the queue


        public:
            Queue( size_t capacity )
                : _nodeArray( nullptr )
                , _capacity(0)
                , _capacityMask(0)
                , _head(0)
                , _tail(0)
            {
                size_t capMaskRef = capacity - 1;
                // set _capacityMask
                while(capMaskRef) {
                    _capacityMask |= capMaskRef;
                    capMaskRef >>=1;
                }
                _capacity = _capacityMask + 1;
                _nodeArray = (Node*)( new uint8_t[ sizeof(Node) * _capacity ] );
                _head.store(0, std::memory_order_relaxed);
                _tail.store(0, std::memory_order_relaxed);
                //
                for( size_t i = 0; i<_capacity; ++i ) {
                    _nodeArray[i].readIndex.store( InvalidPosition, std::memory_order_relaxed );        //> store a value that will never equal to '_head'
                    _nodeArray[i].storeIndex.store( i, std::memory_order_relaxed );                     //> store a value for first cycle( pre-allocate for first 'store' cycle )
                }
            }

            bool push( T val )  
            {
                Node* node = nullptr;
                size_t tailCapture = 0;
                while(true) {
                    tailCapture = _tail.load( std::memory_order_relaxed );                              //> get the capture of '_tail' position
                    node = &_nodeArray[tailCapture&_capacityMask];                                      //> get the pointer of the node
                    size_t storeIndexCapture = node->storeIndex.load( std::memory_order_relaxed );      //> get the node's 'storeIndex'
                    /* ==============================================================================
                    * when we stores a new value, the node's storeIndex must be equal to '_tail', 
                    * otherwise, '_tail' and storeIndexCapture are in different cycle (the queue is full!)
                    * so, it it failed, we should return 'false' immediately
                    * ============================================================================ */
                    if( storeIndexCapture != tailCapture ) {                                              
                        return false;
                    }
                    // update '_tail'
                    if( _tail.compare_exchange_weak( tailCapture, tailCapture + 1, std::memory_order_relaxed ) ) {
                        break;
                    }
                }
                new(&node->val)T(val);
                /* ==================================================================
                *   make the fcuntion call failed if we have not assign the value in the situation
                * that the queue only has the current element
                *   release to ensure constrcuction of Node::val not be reordered after call of store
                * ================================================================== */
                node->readIndex.store( tailCapture, std::memory_order_release ); 
                return true;
            }

            bool pop( T& val ) 
            {
                Node* node = nullptr;
                size_t headCapture = 0;
                while(true) {
                    headCapture = _head.load( std::memory_order_relaxed );
                    node = &_nodeArray[headCapture&_capacityMask];
                    size_t readIndex = node->readIndex.load( std::memory_order_relaxed );
                    /* ==================================================================
                    * similar to store operation, if readIndex not equal to '_head',
                    * readIndex & _head are in different cycle, the queue is empty
                    * ==================================================================*/
                    if( readIndex != headCapture) {
                        return false;
                    }
                    if( _head.compare_exchange_weak( headCapture, headCapture+1, std::memory_order_relaxed ) ) {
                        break;
                    }
                }
                val = std::move(node->val);
                (&node->val)->~T();
                // update the storeIndex for next cycle's store operation
                node->storeIndex.store( headCapture + _capacity, std::memory_order_release );
                return true;
            }
        };
    }
    
}