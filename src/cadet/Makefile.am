# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include

if USE_COVERAGE
  AM_CFLAGS = --coverage -O0
  XLIB = -lgcov
endif

pkgcfgdir= $(pkgdatadir)/config.d/

libexecdir= $(pkglibdir)/libexec/

pkgcfg_DATA = \
  cadet.conf

plugindir = $(libdir)/gnunet

AM_CLFAGS = -g

libexec_PROGRAMS = \
 gnunet-service-cadet \
 $(EXP_LIBEXEC)

bin_PROGRAMS = \
 gnunet-cadet

lib_LTLIBRARIES = \
  libgnunetcadet.la \
  $(EXP_LIB)

libgnunetcadet_la_SOURCES = \
  cadet_api.c \
  cadet_api_get_channel.c \
  cadet_api_get_path.c \
  cadet_api_list_peers.c \
  cadet_api_list_tunnels.c \
  cadet_api_helper.c
libgnunetcadet_la_LIBADD = \
  $(top_builddir)/src/util/libgnunetutil.la \
  $(XLIB) \
  $(LTLIBINTL)
libgnunetcadet_la_LDFLAGS = \
  $(GN_LIB_LDFLAGS)  \
  -version-info 7:0:0

gnunet_cadet_SOURCES = \
  gnunet-cadet.c
gnunet_cadet_LDADD = \
  libgnunetcadet.la \
  $(top_builddir)/src/util/libgnunetutil.la
gnunet_cadet_LDFLAGS = \
  $(GN_LIBINTL)

gnunet_service_cadet_SOURCES = \
 gnunet-service-cadet.c gnunet-service-cadet.h \
 gnunet-service-cadet_channel.c gnunet-service-cadet_channel.h \
 gnunet-service-cadet_connection.c gnunet-service-cadet_connection.h \
 gnunet-service-cadet_core.c gnunet-service-cadet_core.h \
 gnunet-service-cadet_dht.c gnunet-service-cadet_dht.h \
 gnunet-service-cadet_hello.c gnunet-service-cadet_hello.h \
 gnunet-service-cadet_tunnels.c gnunet-service-cadet_tunnels.h \
 gnunet-service-cadet_paths.c gnunet-service-cadet_paths.h \
 gnunet-service-cadet_peer.c gnunet-service-cadet_peer.h
gnunet_service_cadet_LDADD = \
  $(top_builddir)/src/util/libgnunetutil.la \
  $(top_builddir)/src/ats/libgnunetats.la \
  $(top_builddir)/src/core/libgnunetcore.la \
  $(top_builddir)/src/dht/libgnunetdht.la \
  $(top_builddir)/src/statistics/libgnunetstatistics.la \
  $(top_builddir)/src/transport/libgnunettransport.la \
  $(top_builddir)/src/peerinfo/libgnunetpeerinfo.la \
  $(top_builddir)/src/hello/libgnunethello.la \
  $(top_builddir)/src/block/libgnunetblock.la
if LINUX
  gnunet_service_cadet_LDFLAGS = -lrt \
    $(GN_LIBINTL)
endif


if HAVE_TESTING
 noinst_LTLIBRARIES = libgnunetcadettest.la $(noinst_LIB_EXP)
# noinst_PROGRAMS = gnunet-cadet-profiler
endif

if HAVE_TESTING
check_PROGRAMS = \
  test_cadet_local_mq \
  test_cadet_2_forward \
  test_cadet_2_forward \
  test_cadet_2_signal \
  test_cadet_2_keepalive \
  test_cadet_2_speed \
  test_cadet_2_speed_ack \
  test_cadet_2_speed_backwards \
  test_cadet_2_speed_reliable \
  test_cadet_2_speed_reliable_backwards \
  test_cadet_2_reopen \
  test_cadet_5_forward \
  test_cadet_5_signal \
  test_cadet_5_keepalive \
  test_cadet_5_speed \
  test_cadet_5_speed_ack \
  test_cadet_5_speed_reliable \
  test_cadet_5_speed_reliable_backwards \
  test_cadet_5_speed_backwards \
  test_cadet_5_reopen
endif


#gnunet_cadet_profiler_SOURCES = \
#  gnunet-cadet-profiler.c
#gnunet_cadet_profiler_LDADD = $(ld_cadet_test_lib)


test_cadet_local_mq_SOURCES = \
  test_cadet_local_mq.c
test_cadet_local_mq_LDADD = \
  libgnunetcadet.la \
  $(top_builddir)/src/testing/libgnunettesting.la \
  $(top_builddir)/src/util/libgnunetutil.la


libgnunetcadettest_la_SOURCES = \
  cadet_test_lib.c cadet_test_lib.h
libgnunetcadettest_la_LIBADD = \
 $(top_builddir)/src/util/libgnunetutil.la \
 $(top_builddir)/src/testbed/libgnunettestbed.la \
 libgnunetcadet.la

ld_cadet_test_lib = \
  $(top_builddir)/src/util/libgnunetutil.la \
  $(top_builddir)/src/testing/libgnunettesting.la \
  libgnunetcadet.la \
  libgnunetcadettest.la \
  $(top_builddir)/src/testbed/libgnunettestbed.la \
  $(top_builddir)/src/statistics/libgnunetstatistics.la
dep_cadet_test_lib = \
  libgnunetcadet.la \
  libgnunetcadettest.la \
  $(top_builddir)/src/statistics/libgnunetstatistics.la

test_cadet_2_forward_SOURCES = \
  test_cadet.c
test_cadet_2_forward_LDADD = $(ld_cadet_test_lib)

test_cadet_2_signal_SOURCES = \
  test_cadet.c
test_cadet_2_signal_LDADD = $(ld_cadet_test_lib)

test_cadet_2_keepalive_SOURCES = \
  test_cadet.c
test_cadet_2_keepalive_LDADD = $(ld_cadet_test_lib)

test_cadet_2_speed_SOURCES = \
  test_cadet.c
test_cadet_2_speed_LDADD = $(ld_cadet_test_lib)

test_cadet_2_speed_ack_SOURCES = \
  test_cadet.c
test_cadet_2_speed_ack_LDADD = $(ld_cadet_test_lib)

test_cadet_2_speed_backwards_SOURCES = \
  test_cadet.c
test_cadet_2_speed_backwards_LDADD = $(ld_cadet_test_lib)

test_cadet_2_speed_reliable_SOURCES = \
  test_cadet.c
test_cadet_2_speed_reliable_LDADD = $(ld_cadet_test_lib)

test_cadet_2_speed_reliable_backwards_SOURCES = \
  test_cadet.c
test_cadet_2_speed_reliable_backwards_LDADD = $(ld_cadet_test_lib)

test_cadet_5_forward_SOURCES = \
  test_cadet.c
test_cadet_5_forward_LDADD = $(ld_cadet_test_lib)

test_cadet_5_signal_SOURCES = \
  test_cadet.c
test_cadet_5_signal_LDADD = $(ld_cadet_test_lib)

test_cadet_5_keepalive_SOURCES = \
  test_cadet.c
test_cadet_5_keepalive_LDADD = $(ld_cadet_test_lib)

test_cadet_5_speed_SOURCES = \
  test_cadet.c
test_cadet_5_speed_LDADD = $(ld_cadet_test_lib)

test_cadet_5_speed_ack_SOURCES = \
  test_cadet.c
test_cadet_5_speed_ack_LDADD = $(ld_cadet_test_lib)

test_cadet_5_speed_backwards_SOURCES = \
  test_cadet.c
test_cadet_5_speed_backwards_LDADD = $(ld_cadet_test_lib)

test_cadet_5_speed_reliable_SOURCES = \
  test_cadet.c
test_cadet_5_speed_reliable_LDADD = $(ld_cadet_test_lib)

test_cadet_5_speed_reliable_backwards_SOURCES = \
  test_cadet.c
test_cadet_5_speed_reliable_backwards_LDADD = $(ld_cadet_test_lib)

test_cadet_2_reopen_SOURCES = \
  test_cadet.c
test_cadet_2_reopen_LDADD = $(ld_cadet_test_lib)

test_cadet_5_reopen_SOURCES = \
  test_cadet.c
test_cadet_5_reopen_LDADD = $(ld_cadet_test_lib)


if ENABLE_TEST_RUN
AM_TESTS_ENVIRONMENT=export GNUNET_PREFIX=$${GNUNET_PREFIX:-@libdir@};export PATH=$${GNUNET_PREFIX:-@prefix@}/bin:$$PATH;unset XDG_DATA_HOME;unset XDG_CONFIG_HOME;
TESTS = \
 $(check_PROGRAMS)
endif

EXTRA_DIST = \
  cadet.h cadet_protocol.h \
  test_cadet.conf \
  test_cadet_drop.conf
