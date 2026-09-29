# Builds the kwq module and its test module; the if_pair module in the
# parent directory does not depend on this tree until PLAN.txt P4.
SUBDIR=	kwq kwq_test
# make uninstall: remove what make install put in place (both modules, the
# DTrace translator, and refresh linker.hints); unload the modules first.
SUBDIR_TARGETS+=	uninstall
.include <bsd.subdir.mk>
