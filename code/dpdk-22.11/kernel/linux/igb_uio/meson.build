# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2018 Luca Boccassi <bluca@debian.org>

# For SUSE build check function arguments of ndo_tx_timeout API
# Ref: https://jira.devtools.intel.com/browse/DPDK-29263

igb_uio_mkfile = custom_target('igb_uio_makefile',
	output: 'Makefile',
	command: ['touch', '@OUTPUT@'])

igb_uio_sources = files(
	'igb_uio.c',
	'Kbuild',
)

custom_target('igb_uio',
	input: igb_uio_sources,
	output: 'igb_uio.ko',
	command: ['make', '-j4', '-C', kernel_build_dir,
		'M=' + meson.current_build_dir(),
		'src=' + meson.current_source_dir(),
		'modules'] + cross_args,
	depends: igb_uio_mkfile,
	install: install,
#	install_dir: kernel_install_dir,
	install_dir: get_option('prefix') + '/kernel/',
	build_by_default: get_option('enable_kmods'))
