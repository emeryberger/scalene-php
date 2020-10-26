<?php

declare(strict_types=1); // for sanity

namespace SCALENE; // for isolation

include_once("profiler.php");


// parse arguments
for ($i = 1; $i < $argc; $i++)
{
  if ($argv[$i] == "--cpu-only")
  {
    Scalene::$cpu_only = true;
  }
  elseif ($argv[$i] == "--use-virtual-time")
  {
    Scalene::$use_virt_time = true;
  }
  elseif ($argv[$i] == "--cpu-sampling-rate")
  {
    if (++$i == $argc) {
      echo "Insufficient number of arguments!\n";
      exit;
    }
    Scalene::$cpu_sampling_rate = round(floatval($argv[$i]), 6);

    if (Scalene::$cpu_sampling_rate <= 0.0) {
      echo "Invalid CPU sampling rate!\n";
      exit;
    }
  }
  elseif ($argv[$i] == "--malloc-threshold")
  {
    if (++$i == $argc) {
      echo "Insufficient number of arguments!\n";
      exit;
    }
    Scalene::$malloc_threshold = intval($argv[$i]);

    if (Scalene::$malloc_threshold <= 0) {
      echo "Invalid malloc threshold!\n";
      exit;
    }
  }
  else
  {
    Scalene::$profile_target = $argv[$i];
    Scalene::$profile_target_args = array_slice($argv, $i);
    break;
  }
}

if (Scalene::$profile_target === NULL) {
  echo "Insufficient number of arguments!\n";
  exit;
}

// set up the runtime if necessary; otherwise start profiling
if (Scalene::$cpu_only || array_key_exists("LD_PRELOAD", $_ENV))
{
  // open signal files
  if (!Scalene::$cpu_only) {
    Scalene::open_signal_files();
  }

  // set up timer signal
  if (pcntl_setitimer(ITIMER_VIRTUAL,
    Scalene::$cpu_sampling_rate, Scalene::$cpu_sampling_rate) == -1)
  {
    echo "pcntl_setitimer() failed!\n";
    exit;
  }
  Scalene::update_timestamps();

  // update argv & run profiling target
  $argv = Scalene::$profile_target_args;
  Scalene::start();
  include(Scalene::$profile_target);
  Scalene::end();
}
else
{
  // disable PHP's allocator & insert the runtime
  if (!putenv("USE_ZEND_ALLOC=0")) {
    echo "putenv() failed!\n";
    exit;
  }
  if (!putenv("LD_PRELOAD=./libscalene_php.so")) {
    echo "putenv() failed!\n";
    exit;
  }

  // actual run
  $args = array_merge(array("./php"), $argv);
  $status = proc_close(proc_open($args, [STDIN, STDOUT, STDERR], $pipes));

  if ($status != 0) {
    echo "child process exited with $status!\n";
    exit;
  }
}

?>
