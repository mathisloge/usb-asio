option(USB_ASIO_USE_STANDALONE_ASIO "Use standalone asio instead of boost::asio" ON)

add_library(usb_asio INTERFACE)
add_library(usb_asio::usb_asio ALIAS usb_asio)

target_include_directories(usb_asio INTERFACE "include" ${LIBUSB_INCLUDE_DIRS})
target_link_libraries(usb_asio INTERFACE ${LIBUSB_LIBRARIES})

if (USB_ASIO_USE_STANDALONE_ASIO)
  target_compile_definitions(usb_asio INTERFACE "USB_ASIO_USE_STANDALONE_ASIO")
  target_link_libraries(usb_asio INTERFACE asio::asio)
else ()
  target_link_libraries(usb_asio INTERFACE boost::boost)
endif ()
