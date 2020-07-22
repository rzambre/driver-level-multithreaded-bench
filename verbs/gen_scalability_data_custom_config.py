import csv
import argparse
import os
import time
import subprocess
import signal
import math
import sys

'''
In this script, you can explicitly choose inlining, postlist values, unsig values, BF i.e.
you can create your custom configuration to study resource sharing unlike the other script
that fixes the default values for each feature and collects data by turning each feature off
separately.
'''

def make_binary_cmd(args, num_threads, qp_sharing, ctx_sharing, pd_sharing, mr_sharing, buf_sharing, cq_sharing, postlist, unsig):

	bin_cmd = args.benchmark

	if args.device is not None:
		bin_cmd = bin_cmd + " -d " + args.device

	bin_cmd = bin_cmd + " -t " + str(num_threads)

	if args.size is not None:
		bin_cmd = bin_cmd + " -S " + args.size

	if args.TD:
		bin_cmd = bin_cmd + " -o"

	if args.exCQ:
		bin_cmd = bin_cmd + " -x"

	if args.woInl:
		bin_cmd = bin_cmd + " -n"

	bin_cmd = bin_cmd + " -p " + str(postlist)

	bin_cmd = bin_cmd + " -Q " + str(unsig)

	if qp_sharing > num_threads:
		qp_sharing = num_threads
	if ctx_sharing > num_threads:
		ctx_sharing = num_threads
	if pd_sharing > num_threads:
		pd_sharing = num_threads
	if mr_sharing > num_threads:
		mr_sharing = num_threads
	if buf_sharing > num_threads:
		buf_sharing = num_threads
	if cq_sharing > num_threads:
		cq_sharing = num_threads
	
	bin_cmd = bin_cmd + " -E " + str(qp_sharing) + " -X " + str(ctx_sharing) + " -P " + str(pd_sharing) + " -M " + str(mr_sharing) + " -b " + str(buf_sharing) + " -C " + str(cq_sharing)

	return bin_cmd

def make_mpi_cmd(args,  num_threads):

	mpi_cmd = "mpiexec -n 2 -ppn 1 -bind-to core:" + str(num_threads) + " -hosts " + args.node[0] + "," + args.node[1]

	if args.iface is not None:
		mpi_cmd = mpi_cmd + " -iface " + args.iface

	return mpi_cmd

def make_tsv_file_name(args, num_threads, qp_sharing, ctx_sharing, pd_sharing, mr_sharing, buf_sharing, cq_sharing, postlist, unsig, trial):

	tsv_file_name = args.benchmark

	if args.iface is not None:
		tsv_file_name = tsv_file_name + "_" + args.iface

	if args.MLX5SINGLETHREADED:
		tsv_file_name = tsv_file_name + "_wMLX5SINGLETHREADED"

	if args.woBF:
		tsv_file_name = tsv_file_name + "_woBF"

	if args.woInl:
		tsv_file_name = tsv_file_name + "_woInl"

	if args.suffix is not None:
		tsv_file_name = tsv_file_name + "_" + args.suffix

	tsv_file_name = tsv_file_name + "_t" + str(num_threads)

	tsv_file_name = tsv_file_name + "_E" + str(qp_sharing)

	tsv_file_name = tsv_file_name + "_X" + str(ctx_sharing)

	tsv_file_name = tsv_file_name + "_P" + str(pd_sharing)

	tsv_file_name = tsv_file_name + "_M" + str(mr_sharing)

	tsv_file_name = tsv_file_name + "_b" + str(buf_sharing)

	tsv_file_name = tsv_file_name + "_C" + str(cq_sharing)
	
	tsv_file_name = tsv_file_name + "_p" + str(postlist)

	tsv_file_name = tsv_file_name + "_Q" + str(unsig)

	tsv_file_name = tsv_file_name + "_T" + str(trial)

	tsv_file_name = tsv_file_name + ".tsv"

	tsv_file_name = os.path.join(args.dataFolder, "raw-data", tsv_file_name)
	
	return tsv_file_name

def make_summary_file_name(args, prefix):
	
	file_name = prefix

	if args.suffix is not None:
		file_name = file_name + "_" + args.suffix

	if args.MLX5SINGLETHREADED:
		file_name = file_name + "_MLX5SINGLETHREADED"
	
	if args.woBF:
		file_name = file_name + "_woBF"

	if args.woInl:
		file_name = file_name + "_woInl"

	return file_name

def main():
	# defining arguments
	parser = argparse.ArgumentParser()
	parser.add_argument("-threads_list",
						dest="threads_list",
						nargs="+",
						type=int,
						required=True,
						help="list of number of threads to run the benchmark with. Needs to be >1 and a multiple of 2.")
	parser.add_argument("-MLX5SINGLETHREADED",
						action="store_true",
						help="flag to set MLX5_SINGLE_THREADED")
	parser.add_argument("-TD",
						action="store_true",
						help="flag to use Thread Domains")
	parser.add_argument("-exCQ",
						action="store_true",
						help="flag to use extended CQs")
	parser.add_argument("-woBF",
						action="store_true",
						help="flag to turn off BlueFlame")
	parser.add_argument("-woInl",
						action="store_true",
						help="flag to turn off Inlining")
	parser.add_argument("-postlist_list",
						nargs="+",
						dest="postlist_list",
						type=int,
						default=[32],
						help="a list of Postlist")
	parser.add_argument("-unsig_list",
						nargs="+",
						dest="unsig_list",
						type=int,
						default=[64],
						help="a list of Unsignaled Completions")
	parser.add_argument("-E",
						dest="qp_sharing",
						type=int,
						default=1,
						help="x-way QP sharing")
	parser.add_argument("-X",
						dest="ctx_sharing",
						type=int,
						default=1,
						help="x-way CTX sharing")
	parser.add_argument("-P",
						dest="pd_sharing",
						type=int,
						default=1,
						help="x-way PD sharing")
	parser.add_argument("-M",
						dest="mr_sharing",
						type=int,
						default=1,
						help="x-way MR sharing")
	parser.add_argument("-b",
						dest="buf_sharing",
						type=int,
						default=1,
						help="x-way buffer sharing")
	parser.add_argument("-C",
						nargs="+",
						dest="cq_sharing",
						type=int,
						default=1,
						help="x-way CQ sharing")
	parser.add_argument("-n",
						nargs=2,
						dest="node",
						required=True,
						help="the hosts on which to run the benchmark")
	parser.add_argument("-t",
						type=int,
						dest="trials",
						required=True,
						help="number of times to run the benchmark")
	parser.add_argument("-iface",
						dest="iface",
						help="the domain over which to run the test")
	parser.add_argument("-data",
						dest="dataFolder",
						required=True,
						help="the path to the folder that will contain the output tsv files")
	parser.add_argument("-suffix",
						dest="suffix",
						help="an optional suffix string to add to the end of the tsv file names")
	parser.add_argument("-benchmark",
						dest="benchmark",
						required=True,
						help="the benchmark to run")
	parser.add_argument("-d",
						dest="device",
						help="(optional) the Mellanox device to use")
	parser.add_argument("-S",
						dest="size",
						help="(optional) message size to use")

	args = parser.parse_args()
	
	print "Make sure you have set the CPU frequency on both the nodes that you are running on"
	print "Make sure you have the O3 optimiation flag on"
	print "If you haven't set OMP_PLACES=cores, I will set it."
	print "If you haven't set OMP_PROC_BIND=close, I will set it."
	print "Make sure error checking is off in the Makefile"
	print "I will set MLX5_SINGLE_THREADED=1 whenever possible, if you have told me to."

	# CREATE DIRECTORIES
	if not os.path.exists(os.path.join(args.dataFolder)):
		os.makedirs(os.path.join(args.dataFolder, "raw-data"))
		os.makedirs(os.path.join(args.dataFolder, "summary-data"))
	else:
		if not os.path.exists(os.path.join(args.dataFolder, "raw-data")):
			os.makedirs(os.path.join(args.dataFolder, "raw-data"))
		if not os.path.exists(os.path.join(args.dataFolder, "summary-data")):
			os.makedirs(os.path.join(args.dataFolder, "summary-data"))
	
	# COLLECT DATA

	if os.environ.get('OMP_PLACES') == None:
		print "Setting OMP_PLACES=cores"
		os.environ['OMP_PLACES'] = "cores"
	if os.environ.get('OMP_PROC_BIND') == None:
		print "Setting OMP_PROC_BIND=close"
		os.environ['OMP_PROC_BIND'] = "close"

	if args.MLX5SINGLETHREADED:
		if os.environ.get('MLX5_SINGLE_THREADED') is not None:
			print "Unsetting MLX5_SINGLE_THREADED that was set and setting it back"
			del os.environ['MLX5_SINGLE_THREADED']
			os.environ['MLX5_SINGLE_THREADED'] = "1"
		else:
			print "Setting MLX5_SINGLE_THREADED=1"
			os.environ['MLX5_SINGLE_THREADED'] = "1"

	# Deal with BF:
	if args.woBF:
		print "Setting MLX5_SHUT_UP_BF=1"
		os.environ['MLX5_SHUT_UP_BF'] = "1"
	else:
		print "Unsetting MLX5_SHUT_UP_BF, if it is set"
		if os.environ.get('MLX5_SHUT_UP_BF') is not None:
			del os.environ['MLX5_SHUT_UP_BF']

	# iterate over the number of threads:
	for num_threads in args.threads_list:
		# iterate over postlist values:
		for postlist in args.postlist_list:
			# iterate over the unsig values:
			for unsig in args.unsig_list:
				# iterate over the trials:
				for trial in range(1, args.trials + 1):
					# create the binary command:
					binary_cmd = make_binary_cmd(args, num_threads, args.qp_sharing, args.ctx_sharing, args.pd_sharing, args.mr_sharing, args.buf_sharing, args.cq_sharing, postlist, unsig)
					# create the file name:
					tsv_file_name = make_tsv_file_name(args, num_threads, args.qp_sharing, args.ctx_sharing, args.pd_sharing, args.mr_sharing, args.buf_sharing, args.cq_sharing, postlist, unsig, trial)
					# create the mpiexec command:
					mpi_cmd = make_mpi_cmd(args, str(num_threads))
					# create the whole command:
					cmd = mpi_cmd + " ./" + binary_cmd + " > " + tsv_file_name
					# display status:
					print cmd 
					# execute the command:
					subprocess.check_call(cmd, shell=True)
	
	# SUMMARIZE DATA
	# open two output files:
	summary_file_name = make_summary_file_name(args, "alltrials")
	summary_file_name = os.path.join(args.dataFolder, "summary-data", summary_file_name + ".csv")
	summary_file = open(summary_file_name, "wb")
	summary_file_writer = csv.writer(summary_file)
	# write the header for the file:
	summary_header_row = ["num_threads", "qps", "message_size", "qp_sharing", "ctx_sharing", "pd_sharing", "mr_sharing", "buf_sharing", "cq_sharing", "postlist", "unsig", "mr", "bw"]
	summary_file_writer.writerow(summary_header_row) 

	# iterate over the number of threads:
	for num_threads in args.threads_list:
		# iterate over postlist values:
		for postlist in args.postlist_list:
			# iterate over the unsig values:
			for unsig in args.unsig_list:
				# iterate over the trials:
				for trial in range(1, args.trials + 1):
					# create the file name:
					tsv_file_name = make_tsv_file_name(args, num_threads, args.qp_sharing, args.ctx_sharing, args.pd_sharing, args.mr_sharing, args.buf_sharing, args.cq_sharing, postlist, unsig, trial)
					# display status:
					print tsv_file_name
					# open the file:
					tsv_file = open(tsv_file_name, 'rb')
					tsv_file_reader = csv.reader(tsv_file, delimiter='\t')
					# skip the thread_times:
					for i in range(0,num_threads):
						tsv_file_reader.next()
					# skip the blank line:
					tsv_file_reader.next()
					# skip header:
					tsv_file_reader.next()
					# iterate over the remaining lines in the TSV file:
					for row in tsv_file_reader:
						size = row[0].strip()
						threads = row[1].strip()
						if not (threads == str(num_threads) ):
							print "MISMATCH BETWEEN EXPECTED" + str(num_threads) + "AND ACTUAL DATA" +  threads
							sys.exit(0)
						qps = row[2].strip()
						bw = row[4].strip()
						mr = row[5].strip()
						# write_data:
						summary_write_row = [threads, qps, size, args.qp_sharing, args.ctx_sharing, args.pd_sharing, args.mr_sharing, args.buf_sharing, args.cq_sharing, postlist, unsig, mr, bw]
						summary_file_writer.writerow(summary_write_row)
					# close the file
					tsv_file.close()
	# close the output files:
	summary_file.close()
	
if __name__ == '__main__':
	main()
