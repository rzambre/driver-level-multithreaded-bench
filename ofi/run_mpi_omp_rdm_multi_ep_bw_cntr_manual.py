import csv
import argparse
import os
import time
import subprocess
import signal

"""
python run_mpi_rdm_multi_ep_bw_cntr_manual.py -p 2 4 8 16 32 64 -n gomez00 gomez01 -t 10 -iface eth0
"""

def main():
	# defining arguments
	parser = argparse.ArgumentParser()
	parser.add_argument("-p",
						nargs="+",
						dest="processes",
						type=int,
						required=True,
						help="a list of the total number of endpoints to run the benchmark with. Each number needs to be >1 and a multiple of 2.")
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
	#parser.add_argument("-I",
	#					nargs="+",
	#					dest="iterations",
	#					required=True,
	#					help="number of iterations over which to send each byte size")
	parser.add_argument("-iface",
						dest="iface",
						required=True,
						help="the domain over which to run the test")
	parser.add_argument("-data",
						dest="dataFolder",
						required=True,
						help="the path to the folder that will contain the output tsv files")


	args = parser.parse_args()

	# iterate over the number of processes to experiment with:
	for num_eps in args.processes:
		# iterate over the trials:
		for trial in range(1, args.trials + 1):
			# set the cpufrequency for the gomez machine:
			#print "Setting the CPU frequency to 2.5GHz on the gomez machines"
			#subprocess.check_call("/home/rzambre/scripts/cpufreq_set_25Ghz_turbooff.sh &> /dev/null", shell=True)
			# create the binary command:
			binary_cmd = "mpi_omp_rdm_multi_ep_bw_cntr_manual -p sockets -d eth0 --ep-count " + str(num_eps)
			# create the file name:
			tsv_file_name = os.path.join(args.dataFolder, "mpi_omp_rdm_multi_ep_bw_cntr_manual_" + str(num_eps) + "_t" + str(trial) + ".tsv")
			# create the mpiexec command:
			mpi_cmd = "mpiexec -bind-to core:"+ str(num_eps) + " -n 2 -ppn 1 -iface " + args.iface + " -hosts " + args.node[0] + "," + args.node[1] + " ./" + binary_cmd
			# create the whole command:
			cmd = mpi_cmd + " > " + tsv_file_name
			# display status:
			print cmd 
			# execute the command:
			subprocess.check_call(cmd, shell=True)

if __name__ == '__main__':
	main()
