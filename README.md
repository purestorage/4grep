# 4grep

4grep is a tool developed by interns during Summer 2017 at Pure Storage to extend the functionality of zgrep. It makes searching log files faster by having a stored 5gram index file and conducting a 'pre-search'. 4grep works by looking at this stored index file (creates one if it doesn’t exist) and skips the grep process altogether if the search string definitely doesn’t exist. It uses multiple processors to run zgrep concurrently.

It also has a *really* fancy progress bar.

4grep also allows you to use the same flags that grep does. The 4grep program passes these on to grep internally if/when a search occurs.

![alt tag](http://url/to/img.png)

## Contents
* [How the Indexing Works](#how-the-indexing-works)
  * [More Nuance](more-nuance)
* [How to Get it](#how-to-get-it)
* [Usage](#usage)
  * [Simple](#simple)
  * [Advanced Options](#advanced-options)
* [Progress Bar](#progress-bar)
* [Limitations](#limitations)
* [Where is the Index Saved?](#where-is-the-index-saved)
* [4grep Log](#4grep-log)
* [Other Tools Used by 4grep](#other-tools-used-by-4grep)
  * [Zstandard](#zstandard)
  * [xxHash](#xxhash)
* [License](#license)


## How the Indexing Works

A file is indexed whenever it is first encountered. The index is stored based on its full, expanded, de-symlinked path, and once generated, it will never again be re-indexed. The index stores the existence of all 5-grams in a file (sequences of 5 characters).

When searching, 4grep will first parse 5-grams from the regex parameter. If filter strings are given via `--filter`, 5-grams will be generated from them instead. Then, 4grep filters out files that, based on the index, do not contain all of the 5-grams from the parameters. A "normal" search is performed on the files that pass this 5-gram filtering step.

### More Nuance

For every character in a 5-gram, 4grep will apply a 4-bit mask. This drastically reduces the number of possible 5-grams from 2^40 to 2^20, making the index much smaller. It also means that there are collisions. For example, the 5-grams "AAAAA" and "aaaaa" are considered the same. There is a balance between filtering files out more effectively and filtering files out faster, and 5-grams with 4 bits-per-gram happens to be very effective on our log files.


## How to Get It

*Insert instructions here*


## Usage

### Simple
```
$ 4grep <regex> <filelist>
$ find <args> | 4grep <regex>
```

#### Example

```
# searches for files that contain 'STACK'
$ find ~/Desktop/logs/* | ./4grep STACK
4grep filtering on 'STACK'
...
```

### Advanced Options
**--cores**
```
$ 4grep <regex> <filelist> --cores N
```
--cores was added to limit the number of cores that 4grep uses. If not specified, or too large, the program will use the maximum number of cores - 1.

**--indexdir**
```
$ 4grep <regex> <filelist> --indexdir=<location>
```
This option specifies where 4grep stores its index. See [Where is the Index Saved?](#where-is-the-index-saved) for the default index locations.

**--filter**

4grep tries to parse string literals from the provided regex. In the pre-filtering step, it uses its index files to filter out files that don't contain all of these string literals. For example, the regex "Overslept by [0-9]{3}" can only match in files that contain the string literal "Overslept by ". So, 4grep will detect "Overslept by" as a filter string and filter out files that don't contain it in the pre-filtering step.
```
$ 4grep STACKTRACE logs/purestorage.com/fb79-/2017_05_/core.log
4grep filtering on 'STACKTRACE'
...
$ 4grep "STACK.*TRACE" logs/purestorage.com/fb79-/2017_05_/core.log
4grep filtering on ['STACK', 'TRACE']
...
4grep "(STACK|TRACE)" logs/purestorage.com/fb79-/2017_05_/core.log
4grep: cannot detect filter for '(STACK|TRACE)'
```
This auto-detection works for regexes that have their literals at their start and/or end. However, 4grep's only does really basic regex parsing, and in some cases it may help to manually specify string literals to index with. We will call these string literals "filter strings." This can be done with the `--filter` option:
```
$ 4grep --filter <filter_string> <regex> <filelist>
```
You can specify the filter string and regex separately. The filter string will be used to filter any files that have no instances of the string anywhere in the file. The regex is then passed onto grep for these subset of files and will give you the lines which contain the regex.
```
$ 4grep --filter <filter_string_1> --filter <filter_string_2> <regex> <filelist>
```
```
You can also specify multiple filter strings that can be used to create a smaller subset of filtered files
Filter strings must be at least 5 characters
```
```
The filter string must be a literal string, not a regex
```

#### Advanced Examples
```
# Filters files that contain 'WARNING' anywhere then prints out lines that contain a number
$ find ~/Desktop/logs/* | ./4grep --filter WARNING [0-9]
4grep filtering on 'WARNING'
...
# Filters files that contain 'WARNING' and 'STACK' anywhere then prints out lines that contain a number
$ find ~/Desktop/logs/* | ./4grep --filter WARNING --filter STACK [0-9]
4grep filtering on ['WARNING','STACK']
...
# searches for files that contain 'STACK' with at most 10 cores
$ find ~/Desktop/logs/* | ./4grep STACK --cores 10
4grep filtering on 'STACK'
...
# searches for files that contain 'STACK' whilst storing index files in ~/.4grep
$ find ~/Desktop/logs/* | ./4grep STACK --indexdir=~/.4grep
4grep filtering on 'STACK'
...
```



## Progress Bar

Secondly, 4grep includes a progress bar. This is a feature that has become super useful. Here is an example of the progress that is printed to stderr as you run 4grep:
```
>Done: 15.8% of 54752 Elapsed:0m02.4s Bitmapped:100.0% Filtered: 99.9% ETA:0m12.5s
```
| Output        | Meaning       |
| ------------- | ------------- |
| Done          | Indicates the percentage of files that have been searched from the number of files found.|
| File Count    | The number of files found has two colours. Green if all of the files are found, and red if files are still being piped into stdin.|
| Elapsed       | The time since the program began.|
| Bitmapped     | Indicates the proportion of files that had already been indexed. If this is the first time 4grep has seen any of the files (and so no index files exist) this will be 0%. The higher this is, the fewer files 4grep will have to index, and the faster 4grep will be.|
| Filtered      | Indicates the percentage of files that have skipped the grepping process because of the filter. The higher this number, the faster 4grep should be than tgrep since less files will be searched.|
| ETA           | Gives an estimate on how long the program will take to finish. This is calculated from the files already searched and so is only an estimate.|


## Limitations
4grep does not handle file modification. When it filters files out of the search with its filter string, 4grep will consider the state of the file as it was when it was first indexed. If a file is modified to contain a string that is then used as a search index, 4grep may wrongly filter the file out of the search and not report matches within the file. There is not currently any way to re-index a file or directory.

4grep can be bottlenecked by the speed of the filesystem that the index file is stored on, say, NFS.
The smaller the files being searched over, the more significant 4grep's overhead becomes, and the less of a performance improvement it will give.

As described above, 4grep does not parse regular expressions. It only autodetects a filter string for the easy case where string literals are on the left and/or right of the regex.

Strings less than 5 characters cannot be indexed on. Longer strings are best for filtering. The longer the filter string(s), the higher percentage of files should be filtered, and the faster 4grep will go.
4grep wants to go as fast as possible, especially on e.g. penguin. One process per core going through files as fast as it can may bring some machines to their knees. We've had one report of 4grep freezing up a machine searching through a checkout of purity with 40 cores.
 
 
## Where is the Index Saved?
For minimal overhead, we want a global index file shared by everyone. Ideally, log files can be automatically indexed as they are added to fuse, but we do not do this (yet). For now, 4grep has a ranking of directories it would like to store the index in. This ranking goes:
1. `/4gram` (to be used when we get a proper distribution method)
1. `/home/joern/.4gram` (used on penguin at the moment)
1. `~/.cache/4gram` (should fall back to when running most other places)

The index is designed to be persistent, multi-process, multi-user, and multi-machine-on-NFS safe. Though you should just be able to `rm -r` it if the index is just stored in your home directory and 4grep is not running.
The index location may be overridden with the `--indexdir` option.
 
 
## 4grep Log
When 4grep finishes its search, certain statistics will be recorded in 4grep's own log file. This will help to spot patterns between searches and hopefully optimize for the 90% case in the future.
 
 
## Other Tools Used by 4grep

### Zstandard
When storing the index files, Zstandard was chosen as the compression algorithm. The logs are currently stored after being gzipped, but we found that Zstandard performed much better. Since the file would only be compressed once, we did not care too much for this. However, Zstandard outperformed gzip significantly for compression ratios and decompression speeds. We also kept the compression level down at 8 (current max = 22) since we found that for our data, which is small data with mostly 0's, this performed best. More info at: [https://github.com/facebook/zstd](https://github.com/facebook/zstd).

### xxHash
To store the index file, we decided to hash its original name into something more uniform. xxHash, developed by the same author of Zstd (Yann Collet), seemed to be the fastest and easiest to use for our program. More info at: [https://github.com/Cyan4973/xxHash](https://github.com/Cyan4973/xxHash)


## License
