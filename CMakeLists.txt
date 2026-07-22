cmake_minimum_required(VERSION 3.28...3.30)

find_package(Qt6 REQUIRED Widgets)

add_library(obs-grayscale-histogram MODULE)
add_library(OBS::grayscale-histogram ALIAS obs-grayscale-histogram)

target_sources(obs-grayscale-histogram PRIVATE
    obs-grayscale-histogram.c
    grayscale-histogram-dock.cpp
)

target_link_libraries(obs-grayscale-histogram PRIVATE
    OBS::libobs
    OBS::frontend-api
    Qt::Widgets
)

if(OS_WINDOWS)
    configure_file(cmake/windows/obs-module.rc.in obs-grayscale-histogram.rc)
    target_sources(obs-grayscale-histogram PRIVATE obs-grayscale-histogram.rc)
endif()

set_target_properties_obs(obs-grayscale-histogram PROPERTIES
    FOLDER plugins PREFIX "" AUTOMOC ON
)
