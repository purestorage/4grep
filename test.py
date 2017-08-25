from __future__ import print_function

import unittest
import tempfile
import os
import ctypes
import imp
import shutil
import subprocess
import sys

TGREP_DIR = os.path.dirname(os.path.realpath(__file__))
TGREP_FILE = os.path.join(TGREP_DIR, '4grep')

tgrep = imp.load_source('4grep', TGREP_FILE)

TRUNC = 0
MTCH = 1
NO_MTCH = 2

class TestFiltering(unittest.TestCase):
	def setUp(self):
		self.tempdir = tempfile.mkdtemp()
		self.tempindex = tempfile.mkdtemp()

	def tearDown(self):
		shutil.rmtree(self.tempdir)
		shutil.rmtree(self.tempindex)

	def test_filter(self):
		index = tgrep.StringIndex([[str(10 ** tgrep.NGRAM_CHARS)]])
		c_index = index.get_index_struct()
		for i in range(10):
			name = os.path.join(self.tempdir, '{}.txt'.format(i))
			f = open(name, 'w')
			f.write(str(i * 10 ** tgrep.NGRAM_CHARS))
			f.close()

			c_filename = ctypes.c_char_p(name)
			# first run through: no bitmaps, 2nd bit should be set
			ret = tgrep.start_filter(c_index, c_filename, self.tempindex)
			if i == 1:
				self.assertEqual(ret, 3)
			else:
				self.assertEqual(ret, 4)
			# 2nd run through: all bitmaps should be cached, 2nd bit unset
			ret = tgrep.start_filter(c_index, c_filename, self.tempindex)
			if i == 1:
				self.assertEqual(ret, 1)
			else:
				self.assertEqual(ret, 2)

	def test_filter_deletedfiles(self):
		index = tgrep.StringIndex([[str(10 ** tgrep.NGRAM_CHARS)]])
		c_index = index.get_index_struct()
		for i in range(10):
			name = os.path.join(self.tempdir, '{}.txt'.format(i))
			c_filename = ctypes.c_char_p(name)
			# no files exist, so should filter files out
			ret = tgrep.start_filter(c_index, c_filename, self.tempindex)
			self.assertEqual(ret, -1)

	def test_filter_modifiedfiles(self):
		index = tgrep.StringIndex([[str(10 ** tgrep.NGRAM_CHARS)]])
		c_index = index.get_index_struct()
		# write garbage to each file with an old modification time
		for i in range(10):
			name = os.path.join(self.tempdir, '{}.txt'.format(i))
			f = open(name, 'w')
			f.write(str(i * 9 ** tgrep.NGRAM_CHARS))
			f.close()
			os.utime(name, (100, 100))
		# make sure nothing is found when we search
		for i in range(10):
			name = os.path.join(self.tempdir, '{}.txt'.format(i))
			c_filename = ctypes.c_char_p(name)
			ret = tgrep.start_filter(c_index, c_filename, self.tempindex)
			self.assertEqual(ret, 4)
		# modify each file with mtime=real, current time
		for i in range(10):
			name = os.path.join(self.tempdir, '{}.txt'.format(i))
			f = open(name, 'w')
			f.write(str(i * 10 ** tgrep.NGRAM_CHARS))
			f.close()
		# the query should now match file 1.
		for i in range(10):
			name = os.path.join(self.tempdir, '{}.txt'.format(i))
			c_filename = ctypes.c_char_p(name)
			ret = tgrep.start_filter(c_index, c_filename, self.tempindex)
			if i == 1:
				self.assertEqual(ret, 3)
			else:
				self.assertEqual(ret, 4)

class TestIndexAutodetection(unittest.TestCase):
	def test_parsable_chars(self):
		self.assertEqual(
			tgrep.get_index_from_regex('12345.54321'),
			tgrep.StringIndex([['12345', '54321']]))
		self.assertEqual(
			tgrep.get_index_from_regex('12345+54321'),
			tgrep.StringIndex([['12345', '54321']]))
		self.assertEqual(
			tgrep.get_index_from_regex('12345.*54321'),
			tgrep.StringIndex([['12345', '54321']]))

	def test_short(self):
		self.assertEqual(
			tgrep.get_index_from_regex('1234'),
			tgrep.empty_index())
		self.assertEqual(
			tgrep.get_index_from_regex(''),
			tgrep.empty_index())
		self.assertEqual(
			tgrep.get_index_from_regex('1234|4321'),
			tgrep.empty_index())
		self.assertEqual(
			tgrep.get_index_from_regex('1234|4321.*4321'),
			tgrep.empty_index())

	def test_regex_or(self):
		self.assertEqual(
			tgrep.get_index_from_regex('one111|two22|three'),
			tgrep.StringIndex([['one111'], ['two22'], ['three']]))
		self.assertTrue(
			tgrep.get_index_from_regex('one***111|two22|three').empty())
		self.assertEqual(
			tgrep.get_index_from_regex('12345.54321|two22|three'),
			tgrep.StringIndex([['12345', '54321'], ['two22'], ['three']]))

	def test_literal(self):
		self.assertEqual(
			tgrep.get_index_from_regex('qwertyuiop'),
			tgrep.StringIndex([['qwertyuiop']]))

	def test_regex_question_mark(self):
		self.assertTrue(
			tgrep.get_index_from_regex('12345?').empty())

	def test_regex_star(self):
		self.assertTrue(
			tgrep.get_index_from_regex('12345*').empty())

	def test_regex_curly_braces(self):
		self.assertTrue(
			tgrep.get_index_from_regex('12345{0,9}').empty())

class TestStringIndex(unittest.TestCase):
	def test_get_index_struct(self):
		si = tgrep.StringIndex([['aaaaa']])
		struct = si.get_index_struct()
		self.assertEqual(struct.num_rows, 1)
		self.assertEqual(struct.rows[0].length, 1)
		self.assertEqual(struct.rows[0].data[0], 0b00010001000100010001)

		si = tgrep.StringIndex([['aaaaa'], ['bbbbb']])
		struct = si.get_index_struct()
		self.assertEqual(struct.num_rows, 2)
		self.assertEqual(struct.rows[0].length, 1)
		self.assertEqual(struct.rows[0].data[0], 0b00010001000100010001)
		self.assertEqual(struct.rows[1].length, 1)
		self.assertEqual(struct.rows[1].data[0], 0b00100010001000100010)

		si = tgrep.StringIndex([['aaaaa', 'bbbbb'], ['bbbbb']])
		struct = si.get_index_struct()
		self.assertEqual(struct.num_rows, 2)
		self.assertEqual(struct.rows[0].length, 2)
		self.assertEqual(struct.rows[0].data[0], 0b00010001000100010001)
		self.assertEqual(struct.rows[0].data[1], 0b00100010001000100010)

class TestTgrep(unittest.TestCase):
	def setUp(self):
		self.tempdir = tempfile.mkdtemp()

	def tearDown(self):
		shutil.rmtree(self.tempdir)

	def test_tgrep(self):
		index = str(10 ** tgrep.NGRAM_CHARS)
		for i in range(10):
			name = os.path.join(self.tempdir, '{}.txt'.format(i))
			f = open(name, 'w')
			f.write(str(i * 10 ** tgrep.NGRAM_CHARS))
			f.close()
		search = str(10 ** tgrep.NGRAM_CHARS)
		command = "{} {} {}/*.txt".format(
			TGREP_FILE, search, self.tempdir)
		out = subprocess.check_output(command, shell=True)
		self.assertEqual(out.strip(), self.tempdir + '/1.txt:' + search)

	def test_tgrep_or_regex(self):
		str1 = str(10 ** tgrep.NGRAM_CHARS)
		str2 = str(2 * 10 ** tgrep.NGRAM_CHARS)
		search = "{}|{}".format(str1, str2)
		for i in range(10):
			name = os.path.join(self.tempdir, '{}.txt'.format(i))
			f = open(name, 'w')
			f.write(str(i * 10 ** tgrep.NGRAM_CHARS))
			f.close()
		command = "{} -E '{}' {}/*.txt".format(
			TGREP_FILE, search, self.tempdir)
		out = subprocess.check_output(command, shell=True)
		self.assertEqual(out.strip(),
				self.tempdir + '/1.txt:' + str1 + "\n"
				+ self.tempdir + "/2.txt:" + str2)

	def test_tgrep_unindexed(self):
		index = str(10 ** tgrep.NGRAM_CHARS)
		c_index = ctypes.c_char_p(index)
		for i in range(10):
			name = os.path.join(self.tempdir, '{}.txt'.format(i))
			f = open(name, 'w')
			f.write(str(i * 10 ** tgrep.NGRAM_CHARS))
			f.close()
		search = str(10 ** tgrep.NGRAM_CHARS)
		command = "{} --filter='' {} {}/*.txt".format(
			TGREP_FILE, search, self.tempdir)
		out = subprocess.check_output(command, shell=True)
		self.assertEqual(out.strip(), self.tempdir + '/1.txt:' + search)

if __name__ == '__main__':
	unittest.main()
