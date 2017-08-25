#!/usr/bin/env python
from __future__ import print_function
from itertools import izip
from Queue import Queue
from PIL import Image

import multiprocessing
import subprocess
import argparse
import sys
import os

HELP = '''
	This is how you use it.
	'''

def write_bitmaps():
	for filename in os.listdir("/Users/user/Desktop/logs/upstart"):
	    if filename.endswith(".gz"):
	    	proc = subprocess.Popen(["bitmap/exec/generate_bitmap"], stdout=subprocess.PIPE, stdin=subprocess.PIPE)
	    	ret, stderr = proc.communicate(filename)
	    	file = open(filename[0:-3]+'.bin',"w")
	    	file.write(ret)
	    	file.close()

def ratio(im):
	pixels = im.getdata()
	threshold = 100
	count = 0
	for pixel in pixels:
		if pixel > threshold:
			count += 1
	n = len(pixels)
	print('Percentage:{:.2f} Black:{} Size:{}'.format(100.0*count/n, count, n))


class FinalArray(object):
	def __init__(self):
		self.bytelist = bytearray(131072)

def byte_or(bytearray_list, i):
	a = 0
	for bytearray in bytearray_list:
		a = a | bytearray[i]
	return (a,i)

def update_final(result, final):
	a , i = result
	print(i)
	final.bytelist[i] = a

def combine_bitmaps(bytearray_list):
	l = len(bytearray_list[0])
	print(l)
	final = FinalArray()
	for i in range(l):
		r = byte_or(bytearray_list, i)
		update_final(r, final)
	return final

def get_byte_list():
	results = []
	for filename in os.listdir("/Users/user/Desktop/logs/bitmaps"):
		if filename.endswith(".bin"):
			bin_file_tmp = open('../Desktop/logs/bitmaps/' + filename, 'rb')
			results.append(bytearray(bin_file_tmp.read()))
	return results

# a = get_byte_list()
# b = combine_bitmaps(a)
# im = Image.frombytes("1", (1024, 1024), str(b.bytelist))
# im.show()

class Progress(object):
	def __init__(self):
		self.init = 0
		self.curr = 0

def print_progress(progress):
	perc = 100-((progress.curr-1)*100.0/(progress.init-1))
	print('>>{:.1f}%\033[K\033[F'.format(perc), file=sys.stderr)

def start():
	bitmap_queue = Queue()
	l = 131072
	for filename in os.listdir("/Users/user/Desktop/logs/bitmaps"):
		if filename.endswith(".bin"):
			bin_file_tmp = open('../Desktop/logs/bitmaps/' + filename, 'rb')
			ba = bytearray(bin_file_tmp.read())
			bitmap_queue.put(ba)

	prog = Progress()
	prog.init = bitmap_queue.qsize()
	prog.curr = bitmap_queue.qsize()

	while prog.curr > 1:
		print_progress(prog)
		a = bitmap_queue.get()
		b = bitmap_queue.get()
		c = bytearray(l)

		for i in range(l):
			c[i] = a[i] | b[i]
		bitmap_queue.put(c)
		prog.curr += -1

	final = bitmap_queue.get()
	im = Image.frombytes("1", (1024, 1024), str(final))
	im.show()
	ratio(im)

class stdin_iter:
	def __init__(self):
		pass

	def __iter__(self):
		return self

	def next(self):
		ret = sys.stdin.readline().strip()
		if not ret:
			raise StopIteration
		return ret

def main():
	parser = argparse.ArgumentParser("disp_bitmap", usage=HELP, add_help=False)
	parser.add_argument('files', metavar='FILE', type=str, nargs='*')
	args, options = parser.parse_known_args()
	filelist = args.files
	if not filelist:
		filelist = stdin_iter()
	start()

if __name__ == "__main__":
	try:
		main()
	except IOError as e:
		if e.errno == errno.EPIPE:
			pass
	except KeyboardInterrupt:
		pass
