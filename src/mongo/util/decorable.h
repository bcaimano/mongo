/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/**
 * This header describes a mechanism for making "decorable" types.
 *
 * A decorable type is one to which various subsystems may attach subsystem-private data, so long as
 * they declare what that data will be before any instances of the decorable type are created.
 *
 * For example, suppose you had a class Client, representing on a server a network connection to a
 * client process.  Suppose that your server has an authentication module, that attaches data to the
 * client about authentication.  If class Client looks something like this:
 *
 * class Client : public Decorable<Client>{
 * ...
 * };
 *
 * Then the authentication module, before the first client object is created, calls
 *
 *     const auto authDataDescriptor = Client::declareDecoration<AuthenticationPrivateData>();
 *
 * And stores authDataDescriptor in a module-global variable,
 *
 * And later, when it has a Client object, client, and wants to get at the per-client
 * AuthenticationPrivateData object, it calls
 *
 *    authDataDescriptor(client)
 *
 * to get a reference to the AuthenticationPrivateData for that client object.
 *
 * With this approach, individual subsystems get to privately augment the client object via
 * declarations local to the subsystem, rather than in the global client header.
 */

#pragma once

#include <list>

#include "boost/optional.hpp"

#include "mongo/base/checked_cast.h"
#include "mongo/base/global_initializer_registerer.h"
#include "mongo/util/decoration_container.h"
#include "mongo/util/decoration_registry.h"
#include "mongo/util/functional.h"

namespace mongo {

// TODO(CR) this actually belongs in its own class.
template <typename D>
class ComponentConstructable {
public:
    virtual ~ComponentConstructable() = default;

    /**
     * Register a function of this type using  an instance of ConstructorActionRegisterer,
     * below, to cause the function to be executed on new D instances.
     */
    using ConstructorAction = std::function<void(D*)>;

    /**
     * Register a function of this type using an instance of ConstructorActionRegisterer,
     * below, to cause the function to be executed on D instances before they
     * are destroyed.
     */
    using DestructorAction = std::function<void(D*)>;

    /**
     * Representation of a paired ConstructorAction and DestructorAction.
     */
    class ConstructorDestructorActions {
    public:
        ConstructorDestructorActions(ConstructorAction constructor, DestructorAction destructor)
            : _constructor(std::move(constructor)), _destructor(std::move(destructor)) {}

        void onCreate(D* service) const {
            _constructor(service);
        }
        void onDestroy(D* service) const {
            _destructor(service);
        }

    private:
        ConstructorAction _constructor;
        DestructorAction _destructor;
    };

    using ConstructorActionList = std::list<ConstructorDestructorActions>;
    using ConstructorActionListIterator = typename ConstructorActionList::iterator;

    /**
     * Registers a function to execute on new service contexts when they are created, and optionally
     * also register a function to execute before those contexts are destroyed.
     *
     * Construct instances of this type during static initialization only, as they register
     * MONGO_INITIALIZERS.
     */
    class ConstructorActionRegisterer {
    public:
        /**
         * This constructor registers a constructor and optional destructor with the given
         * "name" and no prerequisite constructors or mongo initializers.
         */
        ConstructorActionRegisterer(std::string name,
                                    ConstructorAction constructor,
                                    DestructorAction destructor = {})
            : ConstructorActionRegisterer(
                  std::move(name), {}, std::move(constructor), std::move(destructor)) {}

        /**
         * This constructor registers a constructor and optional destructor with the given
         * "name", and a list of names of prerequisites, "prereqs".
         *
         * The named constructor will run after all of its prereqs successfully complete,
         * and the corresponding destructor, if provided, will run before any of its
         * prerequisites execute.
         */
        ConstructorActionRegisterer(std::string name,
                                    std::vector<std::string> prereqs,
                                    ConstructorAction constructor,
                                    DestructorAction destructor = {})
            : ConstructorActionRegisterer(
                  std::move(name), prereqs, {}, std::move(constructor), std::move(destructor)) {}

        /**
         * This constructor registers a constructor and optional destructor with the given
         * "name", a list of names of prerequisites, "prereqs", and a list of names of dependents,
         * "dependents".
         *
         * The named constructor will run after all of its prereqs successfully complete,
         * and the corresponding destructor, if provided, will run before any of its
         * prerequisites execute. The dependents will run after this constructor and
         * the corresponding destructor, if provided, will run after any of its
         * dependents execute.
         */
        ConstructorActionRegisterer(std::string name,
                                    std::vector<std::string> prereqs,
                                    std::vector<std::string> dependents,
                                    ConstructorAction constructor,
                                    DestructorAction destructor = {});

    private:
        ConstructorActionListIterator _iter;
        boost::optional<GlobalInitializerRegisterer> _registerer;
    };

protected:
    // Note that destructors run in reverse order of constructors, and that failed construction
    // leads to corresponding destructors to run, similarly to how member variable construction and
    // destruction behave.
    void _onCreate() {
        auto& observers = _actions();
        auto observer = observers.begin();
        try {
            for (; observer != observers.end(); ++observer) {
                observer->onCreate(checked_cast<D*>(this));
            }
        } catch (...) {
            _onDestroy(observers.begin(), observer);
            throw;
        }
    }

    void _onDestroy() noexcept {
        auto& observers = _actions();
        _onDestroy(observers.begin(), observers.end());
    }

    void _onDestroy(ConstructorActionListIterator observerBegin,
                    ConstructorActionListIterator observerEnd) noexcept {
        auto observer = observerEnd;
        while (observer != observerBegin) {
            --observer;
            observer->onDestroy(checked_cast<D*>(this));
        }
    }

private:
    /**
     * Accessor function to get the global list of ServiceContext constructor and destructor
     * functions.
     */
    static ConstructorActionList& _actions() {
        static ConstructorActionList cal;
        return cal;
    }
};


template <typename D>
class Decorable : public ComponentConstructable<D> {
    Decorable(const Decorable&) = delete;
    Decorable& operator=(const Decorable&) = delete;

public:
    template <typename T>
    class Decoration {
    public:
        Decoration() = delete;

        T& operator()(D& d) const {
            return static_cast<Decorable&>(d)._decorations.getDecoration(this->_raw);
        }

        T& operator()(D* const d) const {
            return (*this)(*d);
        }

        const T& operator()(const D& d) const {
            return static_cast<const Decorable&>(d)._decorations.getDecoration(this->_raw);
        }

        const T& operator()(const D* const d) const {
            return (*this)(*d);
        }

        const D* owner(const T* const t) const {
            return static_cast<const D*>(getOwnerImpl(t));
        }

        D* owner(T* const t) const {
            return static_cast<D*>(getOwnerImpl(t));
        }

        const D& owner(const T& t) const {
            return *owner(&t);
        }

        D& owner(T& t) const {
            return *owner(&t);
        }

    private:
        const Decorable* getOwnerImpl(const T* const t) const {
            return *reinterpret_cast<const Decorable* const*>(
                reinterpret_cast<const unsigned char* const>(t) - _raw._raw._index);
        }

        Decorable* getOwnerImpl(T* const t) const {
            return const_cast<Decorable*>(getOwnerImpl(const_cast<const T*>(t)));
        }

        friend class Decorable;

        explicit Decoration(
            typename DecorationContainer<D>::template DecorationDescriptorWithType<T> raw)
            : _raw(std::move(raw)) {}

        typename DecorationContainer<D>::template DecorationDescriptorWithType<T> _raw;
    };

    template <typename T>
    static Decoration<T> declareDecoration() {
        return Decoration<T>(getRegistry()->template declareDecoration<T>());
    }

protected:
    Decorable() : _decorations(this, getRegistry()) {}
    ~Decorable() = default;

private:
    static DecorationRegistry<D>* getRegistry() {
        static DecorationRegistry<D>* theRegistry = new DecorationRegistry<D>();
        return theRegistry;
    }

    DecorationContainer<D> _decorations;
};

template <typename D>
class DecorableCopyable {
public:
    template <typename T>
    class Decoration {
    public:
        Decoration() = delete;

        T& operator()(D& d) const {
            return static_cast<DecorableCopyable&>(d)._decorations.getDecoration(this->_raw);
        }

        T& operator()(D* const d) const {
            return (*this)(*d);
        }

        const T& operator()(const D& d) const {
            return static_cast<const DecorableCopyable&>(d)._decorations.getDecoration(this->_raw);
        }

        const T& operator()(const D* const d) const {
            return (*this)(*d);
        }

        const D* owner(const T* const t) const {
            return static_cast<const D*>(getOwnerImpl(t));
        }

        D* owner(T* const t) const {
            return static_cast<D*>(getOwnerImpl(t));
        }

        const D& owner(const T& t) const {
            return *owner(&t);
        }

        D& owner(T& t) const {
            return *owner(&t);
        }

    private:
        const DecorableCopyable* getOwnerImpl(const T* const t) const {
            return *reinterpret_cast<const DecorableCopyable* const*>(
                reinterpret_cast<const unsigned char* const>(t) - _raw._raw._index);
        }

        DecorableCopyable* getOwnerImpl(T* const t) const {
            return const_cast<DecorableCopyable*>(getOwnerImpl(const_cast<const T*>(t)));
        }

        friend class DecorableCopyable;

        explicit Decoration(
            typename DecorationContainer<D>::template DecorationDescriptorWithType<T> raw)
            : _raw(std::move(raw)) {}

        typename DecorationContainer<D>::template DecorationDescriptorWithType<T> _raw;
    };

    template <typename T>
    static Decoration<T> declareDecoration() {
        return Decoration<T>(getRegistry()->template declareDecorationCopyable<T>());
    }

protected:
    DecorableCopyable() : _decorations(this, getRegistry()) {}
    ~DecorableCopyable() = default;

    DecorableCopyable(const DecorableCopyable& other)
        : _decorations(this, getRegistry(), other._decorations) {}
    DecorableCopyable& operator=(const DecorableCopyable& rhs) {
        getRegistry()->copyAssign(&_decorations, &rhs._decorations);
        return *this;
    }

private:
    static DecorationRegistry<D>* getRegistry() {
        static DecorationRegistry<D>* theRegistry = new DecorationRegistry<D>();
        return theRegistry;
    }

    DecorationContainer<D> _decorations;
};

template <typename D>
ComponentConstructable<D>::ConstructorActionRegisterer::ConstructorActionRegisterer(
    std::string name,
    std::vector<std::string> prereqs,
    std::vector<std::string> dependents,
    ConstructorAction constructor,
    DestructorAction destructor) {
    if (!destructor)
        destructor = [](D*) {};
    _registerer.emplace(std::move(name),
                        [this, constructor, destructor](InitializerContext* context) {
                            auto& actions = ComponentConstructable<D>::_actions();
                            _iter = actions.emplace(
                                actions.end(), std::move(constructor), std::move(destructor));
                            return Status::OK();
                        },
                        [this](DeinitializerContext* context) {
                            auto& actions = ComponentConstructable<D>::_actions();
                            actions.erase(_iter);
                            return Status::OK();
                        },
                        std::move(prereqs),
                        std::move(dependents));
}

}  // namespace mongo
