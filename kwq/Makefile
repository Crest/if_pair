# Builds the kwq module and its two test modules.  The if_pair module in
# the parent directory needs kwq.ko (MODULE_DEPEND) and kwq/kwq.h.
SUBDIR=	kwq kwq_test kwq_crypto
# make uninstall: remove what make install put in place (the modules, the
# DTrace translator, and refresh linker.hints); unload the modules first.
SUBDIR_TARGETS+=	uninstall
.include <bsd.subdir.mk>
