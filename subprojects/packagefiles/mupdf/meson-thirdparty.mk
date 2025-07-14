include Makelists

.PHONY: all

ifndef library
$(error library not specified)
endif

all:
	@echo $($(library)_SRC)
	@echo $($(library)_CFLAGS)
	@echo $($(library)_BUILD_CFLAGS)
