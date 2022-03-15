cmake_minimum_required( VERSION 3.21.0 )

project("lockfreequeue")

add_executable(main )

target_sources( main 
PRIVATE
    main.cpp
    # lockfreequeue.cpp
    # lockfreequeue.h
)
