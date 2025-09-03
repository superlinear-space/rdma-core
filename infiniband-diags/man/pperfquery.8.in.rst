==========
pperfquery
==========

---------------------------------------------------------
parallel query InfiniBand port counters on multiple ports
---------------------------------------------------------

:Date: 2025-09-04
:Manual section: 8
:Manual group: Open IB Diagnostics

SYNOPSIS
========

pperfquery [options]

DESCRIPTION
===========

pperfquery is a parallel version of perfquery that queries performance counters
from multiple InfiniBand ports simultaneously using multiple threads. It reads
a list of Port GUIDs from a configuration file and queries each one in parallel,
aggregating the results into a single output file.

Unlike perfquery which queries a single port at a time, pperfquery is designed
for bulk operations where you need to gather performance data from many ports
across your InfiniBand fabric. It uses the same PerfMgt GMPs to obtain
PortCounters (basic performance and error counters) and PortExtendedCounters
from the PMA at each specified node/port.

Note: In PortCounters and PortCountersExtended, components that represent Data
(e.g. PortXmitData and PortRcvData) indicate octets divided by 4 rather than
just octets.

Note: The GUIDs specified in the configuration file are Port GUIDs, which
identify specific ports on InfiniBand devices.

OPTIONS
=======

**-c <file>**
	Configuration file containing Port GUIDs to query (default: conf/pperfquery.conf).
	Each line should contain one GUID in either hexadecimal (0x...) or decimal format.
	Lines starting with # are treated as comments and ignored.

**-o <file>**
	Output file to write results (default: pperfquery_output.txt).

**-x**
	Use extended port counters rather than basic port counters.
	Note that extended port counters attribute is optional.

**-t <timeout>**
	Query timeout in seconds for each individual query (default: 20).

**-n <num>**
	Maximum number of parallel threads to use (default: 10).
	Higher values increase parallelism but may overwhelm target devices.

**-q**
	Quiet mode - suppress MAD warnings and error messages.

**-h**
	Show help message and exit.

CONFIGURATION FILE
==================

The configuration file should contain one Port GUID per line. GUIDs can be
specified in either hexadecimal (0x...) or decimal format. Lines starting
with # are treated as comments and ignored.

Example configuration file format:

::

	# Port GUIDs to query
	0x0002c902004a1b2c
	0x0002c902004a1b2d
	0x0002c902004a1b2e
	# Another port
	0x0002c902004a1b2f

FILES
=====

**conf/pperfquery.conf**
	Default configuration file containing Port GUIDs to query.

**pperfquery_output.txt**
	Default output file for results.

EXAMPLES
========

::

	pperfquery                                    # Use default config and output files
	pperfquery -c my_ports.conf                  # Use custom config file
	pperfquery -o results.txt                    # Use custom output file
	pperfquery -x -n 20                          # Use extended counters with 20 threads
	pperfquery -c ports.conf -o perf_data.txt   # Custom config and output files

OUTPUT FORMAT
=============

The output file contains:

- Header with start time, configuration details, and parameters
- For each GUID: thread ID, GUID, number of ports, and timestamp
- Port counter data for each port (basic or extended depending on -x flag)
- Footer with completion time and total execution time

Example output:

::

	# Parallel perfquery started at Wed Aug 27 00:53:18 2025
	# Config file: conf/pperfquery.conf
	# Number of GUIDs: 4
	# Max threads: 10
	# Extended counters: no
	# Timeout: 20 seconds
	#
	# Thread 1: Querying GUID 0x0002c902004a1b2c with 2 ports at Wed Aug 27 00:53:18 2025
	# Port counters: 0x0002c902004a1b2c port 1 (CapMask: 0x02)
	#	PortXmitData: 0x00000000
	#	PortRcvData: 0x00000000
	#	PortXmitPkts: 0x00000000
	#	PortRcvPkts: 0x00000000
	#
	# Parallel perfquery completed at Wed Aug 27 00:53:18 2025
	# Total time: 0 seconds

PERFORMANCE CONSIDERATIONS
==========================

- **Thread count**: Higher thread counts increase parallelism but may overwhelm
  target devices or cause timeouts. Start with the default (10) and adjust
  based on your fabric size and device capabilities.

- **Timeout**: Adjust the timeout value based on your network latency and
  device response times. Higher values provide more reliability but slower
  overall completion.

- **Batch processing**: The tool processes GUIDs in batches equal to the
  maximum thread count, ensuring controlled parallelism.

ERROR HANDLING
==============

- If a GUID cannot be resolved or queried, the tool continues with other GUIDs
- Failed queries are logged with error details in the output file
- The tool gracefully handles network timeouts and device unavailability
- If no IB devices are available, it runs in simulation mode for testing

AUTHOR
======

SuperLinear Lab
	< tiger1218@foxmail.com >

SEE ALSO
========

perfquery(8), ibstat(8), ibdiag(8)
