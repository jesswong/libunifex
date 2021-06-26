/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <unifex/scope_guard.hpp>
#include <unifex/std_concepts.hpp>
#include <unifex/tag_invoke.hpp>
#include <unifex/detail/any_heap_allocated_storage.hpp>
#include <unifex/detail/type_erasure_builtins.hpp>
#include <unifex/detail/vtable.hpp>
#include <unifex/detail/with_forwarding_tag_invoke.hpp>
#include <unifex/detail/with_type_erased_tag_invoke.hpp>

#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include <unifex/detail/prologue.hpp>

namespace unifex
{
  template <
      std::size_t InlineSize,
      std::size_t InlineAlignment,
      bool RequireNoexceptMove,
      typename DefaultAllocator,
      typename... CPOs>
  struct _any_object {
    class type;
  };

  template <typename T>
  inline constexpr bool _is_in_place_type = false;
  template <typename T>
  inline constexpr bool _is_in_place_type<std::in_place_type_t<T>> = true;

  template <typename T>
  inline constexpr bool _is_any_object_tag_argument =
      _is_in_place_type<T> || std::is_same_v<T, std::allocator_arg_t>;

  template <
      std::size_t InlineSize,
      std::size_t InlineAlignment,
      bool RequireNoexceptMove,
      typename DefaultAllocator,
      typename... CPOs>
  class _any_object<
      InlineSize,
      InlineAlignment,
      RequireNoexceptMove,
      DefaultAllocator,
      CPOs...>::type : private with_type_erased_tag_invoke<type, CPOs>... {
    // Pad size/alignment out to allow storage of at least a pointer.
    static constexpr std::size_t padded_alignment =
        InlineAlignment < alignof(void*) ? alignof(void*) : InlineAlignment;
    static constexpr std::size_t padded_size =
        InlineSize < sizeof(void*) ? sizeof(void*) : InlineSize;

    template <typename T>
    static constexpr bool can_be_stored_inplace_v =
        (sizeof(T) <= padded_size && alignof(T) <= padded_alignment);

    template <typename T>
    static constexpr bool can_be_type_erased_v =
        detail::supports_type_erased_cpos_v<
            T,
            detail::_destroy_cpo,
            detail::_move_construct_cpo<RequireNoexceptMove>,
            CPOs...>;

    using vtable_holder_t = detail::indirect_vtable_holder<
        detail::_destroy_cpo,
        detail::_move_construct_cpo<RequireNoexceptMove>,
        CPOs...>;

  public:
    template(typename T)                                        //
        (requires(!same_as<remove_cvref_t<T>, type>) AND        //
         (!_is_any_object_tag_argument<remove_cvref_t<T>>) AND  //
             can_be_type_erased_v<remove_cvref_t<T>> AND        //
                 constructible_from<remove_cvref_t<T>, T>)      //
        /*implicit*/ type(T&& object) noexcept(
            can_be_stored_inplace_v<remove_cvref_t<T>>&&
                std::is_nothrow_constructible_v<remove_cvref_t<T>, T>)
      : type(std::in_place_type<remove_cvref_t<T>>, static_cast<T&&>(object)) {}

    template(typename T, typename Allocator)                   //
        (requires can_be_type_erased_v<remove_cvref_t<T>> AND  //
             constructible_from<remove_cvref_t<T>, T>)         //
        explicit type(
            std::allocator_arg_t,
            Allocator allocator,
            T&& value) noexcept(can_be_stored_inplace_v<remove_cvref_t<T>>&&
                                    std::is_nothrow_constructible_v<
                                        remove_cvref_t<T>,
                                        T>)
      : type(
            std::allocator_arg,
            std::move(allocator),
            std::in_place_type<remove_cvref_t<T>>,
            static_cast<T&&>(value)) {}

    template(typename T, typename... Args)     //
        (requires can_be_stored_inplace_v<T>)  //
        explicit type(std::in_place_type_t<T>, Args&&... args) noexcept(
            std::is_nothrow_constructible_v<T, Args...>)
      : vtable_(vtable_holder_t::template create<T>()) {
      ::new (static_cast<void*>(&storage_)) T(static_cast<Args&&>(args)...);
    }

    template(typename T, typename... Args)                       //
        (requires can_be_type_erased_v<T> AND                    //
         (!can_be_stored_inplace_v<T>) AND                       //
             std::is_default_constructible_v<DefaultAllocator>)  //
        explicit type(std::in_place_type_t<T>, Args&&... args)
      : type(
            std::allocator_arg,
            DefaultAllocator(),
            std::in_place_type<T>,
            static_cast<Args&&>(args)...) {}

    template(typename T, typename Allocator, typename... Args)  //
        (requires can_be_type_erased_v<T> AND                   //
             can_be_stored_inplace_v<T>)                        //
        explicit type(
            std::allocator_arg_t,
            Allocator,
            std::in_place_type_t<T>,
            Args&&... args) noexcept(std::
                                         is_nothrow_constructible_v<T, Args...>)
      : type(std::in_place_type<T>, static_cast<Args&&>(args)...) {}

    template(typename T, typename Alloc, typename... Args)  //
        (requires can_be_type_erased_v<T> AND               //
         (!can_be_stored_inplace_v<T>))                     //
        explicit type(
            std::allocator_arg_t,
            Alloc alloc,
            std::in_place_type_t<T>,
            Args&&... args)
      : type(
            std::in_place_type<
                detail::any_heap_allocated_storage<T, Alloc, CPOs...>>,
            std::allocator_arg,
            std::move(alloc),
            std::in_place_type<T>,
            static_cast<Args&&>(args)...) {}

    type(const type&) = delete;

    type(type&& other) noexcept(RequireNoexceptMove) : vtable_(other.vtable_) {
      auto* moveConstruct = vtable_->template get<
          detail::_move_construct_cpo<RequireNoexceptMove>>();
      moveConstruct(
          detail::_move_construct_cpo<RequireNoexceptMove>{},
          &storage_,
          &other.storage_);
    }

    ~type() {
      auto* destroy = vtable_->template get<detail::_destroy_cpo>();
      destroy(detail::_destroy_cpo{}, &storage_);
    }

  private:
    friend const vtable_holder_t& get_vtable(const type& self) noexcept {
      return self.vtable_;
    }

    friend void* get_object_address(const type& self) noexcept {
      return const_cast<void*>(static_cast<const void*>(&self.storage_));
    }

    vtable_holder_t vtable_;
    alignas(padded_alignment) std::byte storage_[padded_size];
  };

  template <
      std::size_t InlineSize,
      std::size_t InlineAlignment,
      bool RequireNoexceptMove,
      typename DefaultAllocator,
      typename... CPOs>
  using any_object = typename _any_object<
      InlineSize,
      InlineAlignment,
      RequireNoexceptMove,
      DefaultAllocator,
      CPOs...>::type;

}  // namespace unifex

#include <unifex/detail/epilogue.hpp>