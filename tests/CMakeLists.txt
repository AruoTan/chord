find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Test)

set(kdeconnect_libraries
    kdeconnectcore
    kdeconnectdbusinterfaces
    kdeconnectmodels
    kdeconnectsmshelper
    kdeconnectversion
    KF6::GuiAddons
    KF6::I18n
    KF6::KIOWidgets
    KF6::Notifications
    Qt::DBus
    Qt::Network
    KF6::People
    Qt::Qml
    Qt::Test
)

ecm_add_test(notificationstest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
ecm_add_test(pluginloadtest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
ecm_add_test(sendfiletest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
target_sources(sendfiletest PRIVATE testdevice.cpp)
ecm_add_test(smshelpertest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
ecm_add_test(lanratelimittest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
ecm_add_test(reconnectscheduletest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
ecm_add_test(transfernotificationtest.cpp ../daemon/transfernotificationtracker.cpp
    TEST_NAME transfernotificationtest
    LINK_LIBRARIES Qt::Test KF6::CoreAddons KF6::I18n)
target_include_directories(transfernotificationtest PRIVATE ${CMAKE_SOURCE_DIR})
if (WIN32 OR APPLE)
    ecm_add_test(mdnstest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
endif()

# Do not touch the clipboard of the running session (and avoid a KSystemClipboard deadlock on exit under Wayland)
set_tests_properties(notificationstest pluginloadtest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
