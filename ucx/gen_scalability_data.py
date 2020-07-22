import csv
import argparse
import os
import time
import subprocess
import signal
import math
import sys

'''
This script is to run a multithreaded UCX benchmark
'''

def make_binary_cmd(args, num_threads):

	bin_cmd = args.benchmark

	if args.size is not None:
		bin_cmd = bin_cmd + " -s " + args.size

	bin_cmd = bin_cmd + " -T " + num_threads 

	if args.shared_context:
		bin_cmd = bin_cmd + " -C " + num_threads
	
	if args.shared_worker:
		bin_cmd = bin_cmd + " -W " + num_threads

	return bin_cmd

def make_mpi_cmd(args, num_threads):

	mpi_cmd = "mpiexec -n 2 -ppn 1 -bind-to core:" + num_threads + " -hosts " + args.node[0] + "," + args.node[1]

	return mpi_cmd

def make_tsv_file_name(args, num_threads, trial):

	tsv_file_name = args.benchmark

	if args.suffix is not None:
		tsv_file_name = tsv_file_name + "_" + args.suffix

	tsv_file_name = tsv_file_name + "_T" + num_threads

	if args.shared_context:
		tsv_file_name = tsv_file_name + "_shared_context"
	
	if args.shared_worker:
		tsv_file_name = tsv_file_name + "_shared_worker"

	tsv_file_name = tsv_file_name + "_t" + str(trial)

	tsv_file_name = tsv_file_name + ".tsv"

	tsv_file_name = os.path.join(args.dataFolder, "raw-data", tsv_file_name)
	
	return tsv_file_name

def make_summary_file_name(args, prefix):
	
	file_name = prefix

	if args.suffix is not None:
		file_name = file_name + "_" + args.suffix

	return file_name

def main():
	# defining arguments
	parser = argparse.ArgumentParser()
	parser.add_argument("-threads_list",
						dest="threads_list",
						nargs="+",
						type=int,
						required=True,
						help="a list of the total number of processes. Each number needs to be >1 and a multiple of 2.")
	parser.add_argument("-shared_context",
						action="store_true",
						help="flag to share UCP context between threads")
	parser.add_argument("-shared_worker",
						action="store_true",
						help="flag to share UCP worker between threads")
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
	parser.add_argument("-S",
						dest="size",
						help="(optional) message size to use")
	parser.add_argument("-device_list",
						dest="device_list",
						type=str,
						nargs=1,
						required=True,
						help="the string containing list of devices to set in UCX_NET_DEVICES")
	parser.add_argument("-transport_list",
						dest="transport_list",
						type=str,
						nargs=1,
						required=True,
						help="the string containing list of transports to set in UCX_TLS")
	args = parser.parse_args()
	
	print "Make sure you have set the CPU frequency on both the nodes that you are running on"
	print "Make sure you have the O3 optimiation flag on"
	print "Make sure error checking is off in the Makefile"

	# CREATE DIRECTORIES
	if not os.path.exists(os.path.join(args.dataFolder)):
		os.makedirs(os.path.join(args.dataFolder, "raw-data"))
		os.makedirs(os.path.join(args.dataFolder, "summary-data"))
	else:
		if not os.path.exists(os.path.join(args.dataFolder, "raw-data")):
			os.makedirs(os.path.join(args.dataFolder, "raw-data"))
		if not os.path.exists(os.path.join(args.dataFolder, "summary-data")):
			os.makedirs(os.path.join(args.dataFolder, "summary-data"))

	print "Setting OMP_PLACES=cores"
	os.environ['OMP_PLACES'] = "cores"
	print "Setting OMP_PROC_BIND=close"
	os.environ['OMP_PROC_BIND'] = "close"

	# Set UCX_NET_DEVICES
	print "Setting UCX_NET_DEVICES"
	os.environ['UCX_NET_DEVICES'] = str(args.device_list[0])
	# Set UCX_TLS
	print "Setting UCX_TLS"
	os.environ['UCX_TLS'] = str(args.transport_list[0])
	
	# COLLECT DATA

	# iterate over the number of processes:
	for num_threads in args.threads_list:
		for trial in range(1, args.trials + 1):
			# create the binary command:
			binary_cmd = make_binary_cmd(args, str(num_threads))
			# create the file name:
			tsv_file_name = make_tsv_file_name(args, str(num_threads), trial)
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
	summary_header_row = ["UCX_NET_DEVICES", str(args.device_list[0])]
	summary_file_writer.writerow(summary_header_row) 
	summary_header_row = ["UCX_TLS", str(args.transport_list[0])]
	summary_file_writer.writerow(summary_header_row)
	summary_header_row = ["shared_context", args.shared_context]
	summary_file_writer.writerow(summary_header_row)
	summary_header_row = ["shared_worker", args.shared_worker]
	summary_file_writer.writerow(summary_header_row)
	summary_file_writer.writerow([])
	summary_header_row = ["num_threads", "message_size", "mr"]
	summary_file_writer.writerow(summary_header_row) 

	# iterate over the number of processes:
	for num_threads in args.threads_list:
		# iterate over the trials:
		for trial in range(1, args.trials + 1):
			# create the file name:
			tsv_file_name = make_tsv_file_name(args, str(num_threads), trial)
			# display status:
			print tsv_file_name
			# open the file:
			tsv_file = open(tsv_file_name, 'rb')
			tsv_file_reader = csv.reader(tsv_file, delimiter='\t')
			# skip the header line:
			tsv_file_reader.next()
			# skip the times of each pair:
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
				if not (threads == str(num_threads)):
					print "MISMATCH BETWEEN EXPECTED" + str(num_threads) + "AND ACTUAL DATA" + threads
					sys.exit(0)
				mr = row[2].strip()
				# write_data:
				summary_write_row = [threads, size, mr]
				summary_file_writer.writerow(summary_write_row)
			# close the file
			tsv_file.close()
	# close the output files:
	summary_file.close()
	
if __name__ == '__main__':
	main()
