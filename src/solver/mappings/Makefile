MODULE_NAME = mappings
CHILD_DIRS = actions observations

ifdef HAS_ROOT_MAKEFILE

include .make/template.mk
OBJS_mappings = $(OBJS_action_mappings) $(OBJS_observation_mappings)
include .make/build.mk

else
REDIRECT=$(MODULE_NAME)
.PHONY: $(MAKECMDGOALS) call-upwards
$(MAKECMDGOALS): call-upwards ;
call-upwards:
	@$(MAKE) --no-print-directory -C .. $(MAKECMDGOALS) REDIRECT=$(REDIRECT)
endif
