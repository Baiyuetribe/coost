#include "scheduler.h"
#include <deque>
#include <unordered_set>

namespace co {

class EventImpl {
  public:
    EventImpl() : _counter(0), _signaled(false), _has_cond(false) {}
    ~EventImpl() { co::xx::cond_destroy(&_cond); }

    bool wait(uint32 ms);

    void signal();

  private:
    ::Mutex _mtx;
    co::xx::cond_t _cond;
    std::unordered_set<Coroutine*> _co_wait;
    std::unordered_set<Coroutine*> _co_swap;
    int _counter;
    bool _signaled;
    bool _has_cond;
};

bool EventImpl::wait(uint32 ms) {
    auto s = gSched;
    if (s) { /* in coroutine */
        Coroutine* co = s->running();
        if (co->s != s) co->s = s;
        {
            ::MutexGuard g(_mtx);
            if (_signaled) { if (_counter == 0) _signaled = false; return true; }
            co->state = st_wait;
            _co_wait.insert(co);
        }

        if (ms != (uint32)-1) s->add_timer(ms);
        s->yield();
        if (s->timeout()) {
            ::MutexGuard g(_mtx);
            _co_wait.erase(co);
        }

        co->state = st_init;
        return !s->timeout();

    } else { /* not in coroutine */
        ::MutexGuard g(_mtx);
        if (!_signaled) {
            ++_counter;
            if (!_has_cond) { co::xx::cond_init(&_cond); _has_cond = true; }
            bool r = true;
            if (ms == (uint32)-1) {
                co::xx::cond_wait(&_cond, _mtx.mutex());
            } else {
                r = co::xx::cond_wait(&_cond, _mtx.mutex(), ms);
            }
            --_counter;
            if (!r) return false;
            assert(_signaled);
        }
        if (_counter == 0) _signaled = false;
        return true;
    }
}

void EventImpl::signal() {
    {
        ::MutexGuard g(_mtx);
        if (!_co_wait.empty()) _co_wait.swap(_co_swap);
        if (!_signaled) {
            _signaled = true;
            if (_counter > 0) {
                if (!_has_cond) { co::xx::cond_init(&_cond); _has_cond = true; }
                co::xx::cond_notify(&_cond);
            }
        }
        if (_co_swap.empty()) return;
    }

    // Using atomic operation here, as check_timeout() in the Scheduler 
    // may also modify the state.
    for (auto it = _co_swap.begin(); it != _co_swap.end(); ++it) {
        Coroutine* co = *it;
        if (atomic_compare_swap(&co->state, st_wait, st_ready) == st_wait) {
            ((SchedulerImpl*)co->s)->add_ready_task(co);
        }
    }
    _co_swap.clear();
}

class MutexImpl {
  public:
    MutexImpl() : _lock(false) {}
    ~MutexImpl() = default;

    void lock();

    void unlock();

    bool try_lock();

  private:
    ::Mutex _mtx;
    std::deque<Coroutine*> _co_wait;
    bool _lock;
};

inline bool MutexImpl::try_lock() {
    ::MutexGuard g(_mtx);
    return _lock ? false : (_lock = true);
}

inline void MutexImpl::lock() {
    auto s = gSched;
    CHECK(s) << "must be called in coroutine..";
    _mtx.lock();
    if (!_lock) {
        _lock = true;
        _mtx.unlock();
    } else {
        Coroutine* co = s->running();
        if (co->s != s) co->s = s;
        _co_wait.push_back(co);
        _mtx.unlock();
        s->yield();
    }
}

inline void MutexImpl::unlock() {
    _mtx.lock();
    if (_co_wait.empty()) {
        _lock = false;
        _mtx.unlock();
    } else {
        Coroutine* co = _co_wait.front();
        _co_wait.pop_front();
        _mtx.unlock();
        ((SchedulerImpl*)co->s)->add_ready_task(co);
    }
}

class PoolImpl {
  public:
    typedef std::vector<void*> V;

    PoolImpl()
        : _pools(scheduler_num()), _maxcap((size_t)-1) {
    }

    // @ccb:  a create callback       []() { return (void*) new T; }
    // @dcb:  a destroy callback      [](void* p) { delete (T*)p; }
    // @cap:  max capacity for each pool
    PoolImpl(std::function<void*()>&& ccb, std::function<void(void*)>&& dcb, size_t cap)
        : _pools(scheduler_num()), _maxcap(cap) {
        _ccb = std::move(ccb);
        _dcb = std::move(dcb);
    }

    ~PoolImpl() = default;

    void* pop() {
        auto s = gSched;
        CHECK(s) << "must be called in coroutine..";
        auto& v = _pools[s->id()];
        if (v == NULL) v = this->create_pool();

        if (!v->empty()) {
            void* p = v->back();
            v->pop_back();
            return p;
        } else {
            return _ccb ? _ccb() : 0;
        }
    }

    void push(void* p) {
        if (!p) return; // ignore null pointer

        auto s = gSched;
        CHECK(s) << "must be called in coroutine..";
        auto& v = _pools[s->id()];
        if (v == NULL) v = this->create_pool();

        if (!_dcb || v->size() < _maxcap) {
            v->push_back(p);
        } else {
            _dcb(p);
        }
    }

    size_t size() const {
        auto s = gSched;
        CHECK(s) << "must be called in coroutine..";
        auto& v = _pools[s->id()];
        return v != NULL ? v->size() : 0;
    }

  private:
    // It is not safe to cleanup the pool from outside the Scheduler.
    // So we add a cleanup callback to the Scheduler. It will be called 
    // at the end of Scheduler::loop().
    // TODO: remove cleanup()
    V* create_pool() {
        V* v = new V();
        v->reserve(1024);
        //scheduler()->add_cleanup_cb(std::bind(&PoolImpl::cleanup, v, _dcb));
        return v;
    }

    static void cleanup(V* v, const std::function<void(void*)>& dcb) {
        if (dcb) {
            for (size_t i = 0; i < v->size(); ++i) dcb((*v)[i]);
        }
        delete v;
    }

  private:
    std::vector<V*> _pools;
    std::function<void*()> _ccb;
    std::function<void(void*)> _dcb;
    size_t _maxcap;
};

Event::Event() {
    _p = new EventImpl;
}

Event::~Event() {
    delete (EventImpl*) _p;
}

bool Event::wait(uint32 ms) {
    return ((EventImpl*)_p)->wait(ms);
}

void Event::signal() {
    ((EventImpl*)_p)->signal();
}


Mutex::Mutex() {
    _p = new MutexImpl;
}

Mutex::~Mutex() {
    delete (MutexImpl*) _p;
}

void Mutex::lock() {
    ((MutexImpl*)_p)->lock();
}

void Mutex::unlock() {
    ((MutexImpl*)_p)->unlock();
}

bool Mutex::try_lock() {
    return ((MutexImpl*)_p)->try_lock();
}

Pool::Pool() {
    _p = new PoolImpl;
}

Pool::~Pool() {
    delete (PoolImpl*) _p;
}

Pool::Pool(std::function<void*()>&& ccb, std::function<void(void*)>&& dcb, size_t cap) {
    _p = new PoolImpl(std::move(ccb), std::move(dcb), cap);
}

void* Pool::pop() {
    return ((PoolImpl*)_p)->pop();
}

void Pool::push(void* p) {
    ((PoolImpl*)_p)->push(p);
}

size_t Pool::size() const {
    return ((PoolImpl*)_p)->size();
}

} // co
