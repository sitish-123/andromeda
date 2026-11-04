#pragma once

#include <cstddef>
#include <utility>

namespace andromeda {

  template <class T> class Ref;

  // Base of an object that others point at through Ref<T>. invalidateRefs() nulls every Ref now pointing at it, and the
  // destructor does the same. An owner calls it where it stops being a valid target before destruction (a view when
  // it unmaps).
  class Trackable {
  public:
    Trackable() = default;
    Trackable(const Trackable&) = delete;
    Trackable& operator=(const Trackable&) = delete;
    ~Trackable() { invalidateRefs(); }

    void invalidateRefs() {
      while (m_head != nullptr) {
        Link* link = m_head;
        m_head = link->next;
        if (m_head != nullptr) {
          m_head->prev = nullptr;
        }
        link->prev = nullptr;
        link->next = nullptr;
        link->owner = nullptr;
        link->target = nullptr;
      }
    }

  private:
    struct Link {
      Trackable* owner = nullptr;
      Link* prev = nullptr;
      Link* next = nullptr;
      void* target = nullptr;
    };

    template <class T> friend class Ref;

    void attach(Link* link) {
      link->owner = this;
      link->prev = nullptr;
      link->next = m_head;
      if (m_head != nullptr) {
        m_head->prev = link;
      }
      m_head = link;
    }

    static void detach(Link* link) {
      Trackable* owner = link->owner;
      if (owner == nullptr) {
        return;
      }
      if (link->prev != nullptr) {
        link->prev->next = link->next;
      } else {
        owner->m_head = link->next;
      }
      if (link->next != nullptr) {
        link->next->prev = link->prev;
      }
      link->owner = nullptr;
      link->prev = nullptr;
      link->next = nullptr;
    }

    Link* m_head = nullptr;
  };

  // A pointer that becomes null when its target calls invalidateRefs() or is destroyed. T must derive from Trackable
  // where a Ref is assigned from a pointer; the class may stay incomplete elsewhere.
  template <class T> class Ref {
  public:
    Ref() = default;
    Ref(T* target) : Ref() { assign(target); }
    Ref(std::nullptr_t) : Ref() {}
    Ref(const Ref& other) : Ref() { assignFrom(other); }
    Ref(Ref&& other) noexcept : Ref() { assignFrom(other); }
    Ref& operator=(const Ref& other) {
      if (this != &other) {
        assignFrom(other);
      }
      return *this;
    }
    Ref& operator=(Ref&& other) noexcept {
      if (this != &other) {
        assignFrom(other);
      }
      return *this;
    }
    Ref& operator=(T* target) {
      assign(target);
      return *this;
    }
    Ref& operator=(std::nullptr_t) {
      reset();
      return *this;
    }
    ~Ref() { Trackable::detach(&m_link); }

    [[nodiscard]] T* get() const { return static_cast<T*>(m_link.target); }
    [[nodiscard]] T* operator->() const { return get(); }
    [[nodiscard]] T& operator*() const { return *get(); }
    explicit operator bool() const { return m_link.target != nullptr; }
    void reset() {
      Trackable::detach(&m_link);
      m_link.target = nullptr;
    }

    friend bool operator==(const Ref& ref, const T* other) { return ref.get() == other; }
    friend bool operator==(const Ref& a, const Ref& b) { return a.get() == b.get(); }
    friend bool operator==(const Ref& ref, std::nullptr_t) { return ref.get() == nullptr; }

  private:
    void assign(T* target) {
      Trackable::detach(&m_link);
      m_link.target = target;
      if (target != nullptr) {
        static_cast<Trackable*>(target)->attach(&m_link);
      }
    }

    void assignFrom(const Ref& other) { assign(other.get()); }

    Trackable::Link m_link;
  };

} // namespace andromeda
