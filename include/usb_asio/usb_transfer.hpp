#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <ranges>
#include <span>
#include <vector>

#include <boost/asio/async_result.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <libusb.h>
#include "usb_asio/error.hpp"
#include "usb_asio/usb_device.hpp"

namespace usb_asio
{
    class usb_control_transfer_buffer
    {
      public:
        explicit usb_control_transfer_buffer(std::size_t const size)
            : usb_control_transfer_buffer{size, std::pmr::get_default_resource()} {}
        usb_control_transfer_buffer(
            std::size_t const size,
            std::pmr::memory_resource* const mem_resource)
          : data_(((size - 1u) / 2u) + 1u + LIBUSB_CONTROL_SETUP_SIZE, mem_resource) { }

        [[nodiscard]] auto payload() noexcept -> std::span<std::byte>
        {
            return std::as_writable_bytes(std::span{data_})
                .subspan(LIBUSB_CONTROL_SETUP_SIZE);
        }

        [[nodiscard]] auto payload() const noexcept -> std::span<std::byte const>
        {
            return std::as_bytes(std::span{data_})
                .subspan(LIBUSB_CONTROL_SETUP_SIZE);
        }

        [[nodiscard]] auto data() noexcept -> std::byte*
        {
            return payload().data();
        }

        [[nodiscard]] auto data() const noexcept -> std::byte const*
        {
            return payload().data();
        }

        [[nodiscard]] auto size() const noexcept -> std::size_t
        {
            return payload().size();
        }

      private:
        std::pmr::vector<std::uint16_t> data_;
    };

    inline constexpr auto usb_no_timeout = std::chrono::milliseconds{0};

    struct usb_iso_packet_transfer_result
    {
        std::size_t transferred;
        std::error_code ec;
    };

    template <
        usb_transfer_type transfer_type,
        usb_transfer_direction transfer_direction>
    struct usb_transfer_traits
    {
        using result_type = std::size_t;
        struct result_storage_type
        {
        };
    };

    template <usb_transfer_direction transfer_direction>
    struct usb_transfer_traits<usb_transfer_type::isochronous, transfer_direction>
    {
        using result_type = std::span<usb_iso_packet_transfer_result const>;
        using result_storage_type = std::vector<usb_iso_packet_transfer_result>;
    };

    template <
        usb_transfer_type transfer_type_,
        usb_transfer_direction transfer_direction_,
        typename Executor = boost::asio::executor>
    class basic_usb_transfer
    {
      public:
        using handle_type = ::libusb_transfer*;
        using unique_handle_type = libusb_ptr<::libusb_transfer, &::libusb_free_transfer>;
        using executor_type = Executor;
        using traits_type = usb_transfer_traits<transfer_type_, transfer_direction_>;
        using result_type = typename traits_type::result_type;
        using completion_handler_sig = void(std::error_code, result_type);

        static constexpr auto transfer_type = transfer_type_;
        static constexpr auto transfer_direction = transfer_direction_;

        // clang-format off
        template <typename OtherExecutor>
        basic_usb_transfer(
            executor_type& executor,
            basic_usb_device<OtherExecutor>& device,
            std::chrono::milliseconds const timeout = usb_no_timeout,
            std::pmr::memory_resource* const mem_resource = std::pmr::get_default_resource())
        requires (transfer_type == usb_transfer_type::control)
          // clang-format on
          : handle_{::libusb_alloc_transfer(0)}
          , completion_context_{
                std::make_unique<CompletionContext>(CompletionContext{executor}),
            }
        {
            check_is_constructed();

            ::libusb_fill_control_transfer(
                handle(),
                device.handle(),
                nullptr,
                &completion_callback,
                completion_context_.get(),
                static_cast<unsigned>(timeout.count()));
        }

        // clang-format off
        template <typename OtherExecutor, typename PacketSizeRange>
        basic_usb_transfer(
            executor_type& executor,
            basic_usb_device<OtherExecutor>& device,
            std::uint8_t const endpoint,
            PacketSizeRange&& packet_sizes,
            std::chrono::milliseconds const timeout = usb_no_timeout,
            std::pmr::memory_resource* const mem_resource = std::pmr::get_default_resource())
        requires (transfer_type == usb_transfer_type::isochronous)
            && std::ranges::input_range<PacketSizeRange>
            && std::ranges::sized_range<PacketSizeRange>
            && std::unsigned_integral<std::ranges::range_value_t<PacketSizeRange>>
          // clang-format on
          : handle_{::libusb_alloc_transfer(static_cast<int>(std::ranges::size(packet_sizes)))},
            completion_context_{
                std::make_unique<CompletionContext>(CompletionContext{executor}),
            }
        {
            check_is_constructed();

            auto const num_packets = std::ranges::size(packet_sizes);
            completion_context_->result_storage.resize(num_packets);

            auto packet = 0;
            for (auto const packet_size : packet_sizes)
            {
                handle()->iso_packet_desc[packet++].length = static_cast<unsigned>(packet_size);
            }

            ::libusb_fill_iso_transfer(
                handle(),
                device.handle(),
                endpoint,
                nullptr,
                0,
                static_cast<int>(num_packets),
                &completion_callback,
                completion_context_.get(),
                static_cast<unsigned>(timeout.count()));
        }

        // clang-format off
        template <typename OtherExecutor>
        basic_usb_transfer(
            executor_type& executor,
            basic_usb_device<OtherExecutor>& device,
            std::uint8_t const endpoint,
            std::chrono::milliseconds const timeout = usb_no_timeout,
            std::pmr::memory_resource* const mem_resource = std::pmr::get_default_resource())
        requires (transfer_type == usb_transfer_type::bulk)
          // clang-format on
          : handle_{::libusb_alloc_transfer(0)}
          , completion_context_{
                std::make_unique<CompletionContext>(CompletionContext{executor}),
            }
        {
            check_is_constructed();

            ::libusb_fill_bulk_transfer(
                handle(),
                device.handle(),
                endpoint,
                nullptr,
                0,
                &completion_callback,
                completion_context_.get(),
                static_cast<unsigned>(timeout.count()));
        }

        // clang-format off
        template <typename OtherExecutor>
        basic_usb_transfer(
            executor_type& executor,
            basic_usb_device<OtherExecutor>& device,
            std::uint8_t const endpoint,
            std::chrono::milliseconds const timeout = usb_no_timeout,
            std::pmr::memory_resource* const mem_resource = std::pmr::get_default_resource())
        requires (transfer_type == usb_transfer_type::interrupt)
          // clang-format on
          : handle_{::libusb_alloc_transfer(0)}
          , completion_context_{
                std::make_unique<CompletionContext>(CompletionContext{executor}),
            }
        {
            check_is_constructed();

            ::libusb_fill_interrupt_transfer(
                handle(),
                device.handle(),
                endpoint,
                nullptr,
                0,
                &completion_callback,
                completion_context_.get(),
                static_cast<unsigned>(timeout.count()));
        }

        // clang-format off
        template <typename OtherExecutor>
        basic_usb_transfer(
            executor_type& executor,
            basic_usb_device<OtherExecutor>& device,
            std::uint8_t const endpoint,
            std::uint32_t const stream_id,
            std::chrono::milliseconds const timeout = usb_no_timeout,
            std::pmr::memory_resource* const mem_resource = std::pmr::get_default_resource())
        requires (transfer_type == usb_transfer_type::bulk_stream)
          // clang-format on
          : handle_{::libusb_alloc_transfer(0)}
          , completion_context_{
                std::make_unique<CompletionContext>(CompletionContext{executor}),
            }
        {
            check_is_constructed();

            ::libusb_fill_bulk_stream_transfer(
                handle(),
                device.handle(),
                endpoint,
                stream_id,
                nullptr,
                0,
                &completion_callback,
                completion_context_.get(),
                static_cast<unsigned>(timeout.count()));
        }

        [[nodiscard]] auto handle() const noexcept -> handle_type
        {
            return handle_.get();
        }

        void cancel()
        {
            try_with_ec([&](auto& ec) {
                cancel(ec);
            });
        }

        void cancel(std::error_code& ec) noexcept
        {
            libusb_try(ec, ::libusb_cancel_transfer, handle());
        }

        // clang-format off
        template <typename CompletionToken = boost::asio::default_completion_token_t<executor_type>>
        auto async_read_some(boost::asio::mutable_buffer const buffer, CompletionToken&& token = {})
        requires (transfer_direction == usb_transfer_direction::in)
            && (transfer_type != usb_transfer_type::control)
        // clang-format on
        {
            handle()->buffer = static_cast<unsigned char*>(buffer.data());
            handle()->length = static_cast<int>(buffer.size());

            return async_submit_impl(std::forward<CompletionToken>(token));
        }

        // clang-format off
        template <typename CompletionToken = boost::asio::default_completion_token_t<executor_type>>
        auto async_write_some(boost::asio::const_buffer const buffer, CompletionToken&& token = {})
        requires (transfer_direction == usb_transfer_direction::out)
            && (transfer_type != usb_transfer_type::control)
        // clang-format on
        {
            handle()->buffer = static_cast<unsigned char*>(const_cast<void*>(buffer.data()));
            handle()->length = static_cast<int>(buffer.size());

            return async_submit_impl(std::forward<CompletionToken>(token));
        }

        // clang-format off
        template <typename CompletionToken = boost::asio::default_completion_token_t<executor_type>>
        auto async_control(
            usb_control_request_recipient const recipient,
            usb_control_request_type const type,
            std::uint8_t const request,
            std::uint16_t const value,
            std::uint8_t const index,
            usb_control_transfer_buffer& buffer,
            CompletionToken&& token = {})
        requires (transfer_type == usb_transfer_type::control)
        // clang-format on
        {
            handle()->buffer = reinterpret_cast<unsigned char*>(buffer.data());
            handle()->length = static_cast<int>(buffer.size() + LIBUSB_CONTROL_SETUP_SIZE);

            ::libusb_fill_control_setup(
                reinterpret_cast<unsigned char*>(buffer.data() - LIBUSB_CONTROL_SETUP_SIZE),
                static_cast<std::uint8_t>(
                    static_cast<unsigned>(recipient)
                    | static_cast<unsigned>(type)
                    | static_cast<unsigned>(transfer_direction)),
                request,
                value,
                index,
                buffer.size());

            return async_submit_impl(std::forward<CompletionToken>(token));
        }

      private:
        struct CompletionContext
        {
            executor_type executor;
            [[no_unique_address]] typename traits_type::result_storage_type result_storage = {};
            std::optional<boost::asio::executor_work_guard<executor_type>>
                work_guard = std::nullopt;
            std::function<completion_handler_sig> completion_handler = {};
        };

        unique_handle_type handle_;
        std::unique_ptr<CompletionContext> completion_context_;

        static void completion_callback(handle_type const handle) noexcept
        {
            auto const ec = std::error_code{
                static_cast<usb_transfer_errc>(handle->status),
            };
            auto& context = *static_cast<CompletionContext*>(handle->user_data);

            auto const result = [&]() {
                if constexpr (transfer_type == usb_transfer_type::isochronous)
                {
                    std::ranges::transform(
                        std::span{
                            handle->iso_packet_desc,
                            static_cast<std::size_t>(handle->num_iso_packets),
                        },
                        context.result_storage.begin(),
                        [](auto const& packet_desc) {
                            return usb_iso_packet_transfer_result{
                                static_cast<std::size_t>(packet_desc.actual_length),
                                static_cast<usb_transfer_errc>(packet_desc.status),
                            };
                        });
                }
                else
                {
                    return handle->actual_length;
                }
            }();

            boost::asio::post(
                context.executor,
                std::bind_front(
                    std::move(context.completion_handler),
                    ec,
                    result));
            context.work_guard.reset();
        }

        template <typename CompletionToken>
        auto async_submit_impl(CompletionToken&& token)
        {
            auto completion = boost::asio::async_completion<
                CompletionToken,
                completion_handler_sig>{token};

            completion_context_->work_guard.emplace(completion_context_->executor);
            completion_context_->completion_handler = completion.completion_handler;

            auto ec = std::error_code{};
            libusb_try(ec, &::libusb_submit_transfer, handle());

            if (ec)
            {
                boost::asio::post(
                    completion_context_->executor,
                    std::bind_front(
                        std::move(completion_context_->completion_handler),
                        ec,
                        result_type{}));
                completion_context_->work_guard.reset();
            }

            return completion.result.get();
        }

        void check_is_constructed() const
        {
            if (handle_ == nullptr)
            {
                throw std::bad_alloc{};
            }
        }
    };
}  // namespace usb_asio