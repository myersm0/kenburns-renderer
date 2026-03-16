cmake_minimum_required(VERSION 3.16)
project(kenburns-renderer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui imgcodecs)
find_package(Threads REQUIRED)

add_executable(kbr
	src/main.cpp
)

target_include_directories(kbr PRIVATE
	src
)

target_link_libraries(kbr PRIVATE
	${OpenCV_LIBS}
	Threads::Threads
)


add_executable(test_keyframe
	test/test_keyframe.cpp
)

target_include_directories(test_keyframe PRIVATE
	src
)

enable_testing()
add_test(NAME keyframe COMMAND test_keyframe)
