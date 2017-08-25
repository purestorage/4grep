import os
import subprocess

def set_params(n, b):
	f = open("./bitmap/src/util.h")
	lines = list(f.readlines())
	n_set = b_set = False
	for i, line in enumerate(lines):
		if not n_set and 'NGRAM_CHARS' in line:
			lines[i] = "#define NGRAM_CHARS {}\n".format(n)
			n_set = True
			if b_set:
				break
		elif not b_set and 'NGRAM_CHAR_BITS' in line:
			lines[i] = '#define NGRAM_CHAR_BITS {}\n'.format(b)
			b_set = True
			if n_set:
				break
	f.close()
	with open('./bitmap/src/util.h', 'w') as f:
		f.writelines(lines)

def test_params(n, b):
	set_params(n, b)
	subprocess.check_call('make')
	subprocess.check_call('rm -rf ~/.cache/4gram', shell=True)
	search = 'May 10 12:12:12'
	print('{} {}'.format(n, b))
	for i in range(2):
		p = subprocess.Popen('find /home/mpfeiffer/logs/remote_logs -name "*.gz" -type f | 4grep --index="{}" "{}" > /dev/null'.format(search, search), shell=True, stderr=subprocess.PIPE)
		output = p.communicate()[1]
		lines = output.split('\n')
		lastline = output.split('\n')[-3]
		print(lastline)
	print(subprocess.check_output('du -h ~/.cache/4gram/packfile')

for n in range(2, 10 + 1):
	for b in range(1, min(31 / n + 1, 8 + 1)):
		test_params(n, b)
