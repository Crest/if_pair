PORTNAME=	if_pair
DISTVERSION=	0.1.0
CATEGORIES=	net
PKGNAMESUFFIX=	-kmod

MAINTAINER=	crest+freebsd@rlwinm.de
COMMENT=	Point-to-point layer-3 interface pairs for vnet jails
#WWW=		https://TODO/if_pair

LICENSE=	BSD2CLAUSE
LICENSE_FILE=	${WRKSRC}/LICENSE

USES=		kmod uidfix

# TODO: no public repository/release exists yet.  Once one does, switch
# to USE_GITHUB (or a MASTER_SITES tarball) and run `make makesum`:
#USE_GITHUB=	yes
#GH_ACCOUNT=	TODO

OPTIONS_DEFINE=	DOCS

PLIST_FILES=	${KMODDIR}/if_pair.ko \
		share/man/man4/if_pair.4.gz
PORTDOCS=	README.md NOTES.md

post-install:
	${INSTALL_MAN} ${WRKSRC}/if_pair.4 \
		${STAGEDIR}${PREFIX}/share/man/man4/

post-install-DOCS-on:
	${MKDIR} ${STAGEDIR}${DOCSDIR}
	${INSTALL_DATA} ${WRKSRC}/README.md ${WRKSRC}/NOTES.md \
		${STAGEDIR}${DOCSDIR}

.include <bsd.port.mk>
