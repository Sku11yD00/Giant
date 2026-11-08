giant_module(audio
  SOURCES
    src/mixer.cpp
    src/propagation.cpp
    src/sounds.cpp
  DEPENDS core
)

giant_module_tests(audio
  SOURCES
    tests/test_audio.cpp
)
