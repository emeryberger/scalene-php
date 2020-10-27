<?php

declare(strict_types=1); // for sanity

namespace SCALENE; // for isolation


final class Scalene
{
  public static bool $cpu_only = false;
  public static bool $use_virt_time = false;
  public static float $cpu_sampling_rate = 0.01; // sec
  public static int $malloc_threshold = 100; // # of samples
  public static ?string $profile_target = NULL;
  public static ?array $profile_target_args = NULL;

  private static $alloc_signal_file = NULL;
  private static $memcpy_signal_file = NULL;
  private static int $alloc_signal_file_pos = 0;
  private static int $memcpy_signal_file_pos = 0;

  private static array $cpu_samples_php = array();
  private static array $cpu_samples_c = array();
  private static array $malloc_samples = array();
  private static array $malloc_samples_php = array();
  private static array $malloc_counters = array(); // per file counters
  private static array $free_samples = array();
  private static array $memcpy_samples = array();

  private static ?string $thread_id = NULL;
  private static bool $in_signal_handler = false;
  private static float $last_signal_time_virt = 0.0;
  private static float $current_footprint = 0.0;
  private static float $max_footprint = 0.0;
  private static float $total_allocs = 0.0;
  private static float $total_free = 0.0;
  private static float $total_copy = 0.0;

  public static function signal_dispatch(int $signo)
  {
    // avoid self-recursion
    if (self::$in_signal_handler) {
      return;
    } else {
      self::$in_signal_handler = true;
    }

    switch ($signo) {
      case SIGALRM: // wall clock time
        // echo "SIGALRM received\n";
        self::cpu_signal_handler();
        break;
      case SIGVTALRM: // CPU time
        // echo "SIGVTALRM received\n";
        self::cpu_signal_handler();
        break;
      case SIGXCPU: // malloc
        // echo "SIGXCPU received\n";
        self::alloc_signal_handler();
        break;
      case SIGXFSZ: // free
        // echo "SIGXFSZ received\n";
        self::alloc_signal_handler();
        break;
      case SIGPROF: // memcpy
        // echo "SIGPROF received\n";
        self::memcpy_signal_handler();
        break;
      default:
        echo "signal $signo received\n";
        exit;
    }

    // reset
    self::$in_signal_handler = false;
  }

  private static function cpu_signal_handler()
  {
    // get elapsed times
    $elapsed_virt = self::get_process_time() - self::$last_signal_time_virt;

    // get stack trace
    $trace = debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS);
    if (!self::should_trace($trace)) {
      return;
    }

    // save samples
    $c_time = $elapsed_virt - self::$cpu_sampling_rate;
    if ($c_time < 0.0) {
      $c_time = 0;
    }
    $entry = $trace[1]["file"] . ":" . strval($trace[1]["line"]);

    if (array_key_exists($entry, self::$cpu_samples_php)) {
      self::$cpu_samples_php[$entry] += self::$cpu_sampling_rate;
      self::$cpu_samples_c[$entry] += $c_time;
    } else {
      self::$cpu_samples_php[$entry] = self::$cpu_sampling_rate;
      self::$cpu_samples_c[$entry] = $c_time;
    }

    // avoid time skew
    self::update_timestamps();
  }

  private static function alloc_signal_handler()
  {
    // get stack trace
    $trace = debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS);
    if (!self::should_trace($trace)) {
      return;
    }

    // read data from the runtime
    if (fseek(self::$alloc_signal_file, self::$alloc_signal_file_pos)) {
      echo "fseek() failed!\n";
      exit;
    }

    $data = array();
    while (($line = fgets(self::$alloc_signal_file)) !== false) {
      if ($line === "\n") {
        break; // end marker reached
      }

      // each element = [action, timestamp, size, php_fraction]
      $arr = explode(",", $line);
      if ($arr[0] === self::$thread_id) {
        $data[] = $arr;
      }
      self::$alloc_signal_file_pos += (strlen($line));
    }

    // calculate & record stats
    if (sort($data) === false) {
      echo "failed to sort data!\n";
      exit;
    }

    $allocs = 0.0;
    $php_allocs = 0.0;
    $before = self::$current_footprint;

    foreach ($data as $entry) {
      $is_malloc = ($entry[1] === "M");
      // $timestamp = intval($entry[2]);
      $size = floatval($entry[3]) / (1024 * 1024);
      $php_fraction = floatval($entry[4]);

      if ($is_malloc) {
        self::$current_footprint += $size;
        $allocs += $size;
        $php_allocs += $php_fraction * $size;

        if (self::$current_footprint > self::$max_footprint) {
          self::$max_footprint = self::$current_footprint;
        }
      } else {
        self::$current_footprint -= $size;
      }
    }

    $net = self::$current_footprint - $before;
    self::$total_allocs += $allocs;
    self::$total_free += ($allocs - $net);

    // save samples
    $entry = $trace[1]["file"] . ":" . strval($trace[1]["line"]);
    if ($net > 0)
    {
      if (array_key_exists($entry, self::$malloc_samples)) {
        self::$malloc_samples[$entry] += $net;
        self::$malloc_samples_php[$entry] += ($php_allocs / $allocs) * $net;
      } else {
        self::$malloc_samples[$entry] = $net;
        self::$malloc_samples_php[$entry] = ($php_allocs / $allocs) * $net;
      }

      if (array_key_exists($trace[1]["file"], self::$malloc_counters)) {
        self::$malloc_counters[$entry] += 1;
      } else {
        self::$malloc_counters[$entry] = 1;
      }
    }
    else
    {
      if (array_key_exists($entry, self::$free_samples)) {
        self::$free_samples[$entry] -= $net;
      } else {
        self::$free_samples[$entry] = -$net;
      }
    }
  }

  private static function memcpy_signal_handler()
  {
    // get stack trace
    $trace = debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS);
    if (!self::should_trace($trace)) {
      return;
    }

    // read data from the runtime
    if (fseek(self::$memcpy_signal_file, self::$memcpy_signal_file_pos)) {
      echo "fseek() failed!\n";
      exit;
    }

    $data = array();
    while (($line = fgets(self::$memcpy_signal_file)) !== false) {
      if ($line === "\n") {
        break; // end marker reached
      }

      // each element = [timestamp, size]
      $arr = explode(",", $line);
      if ($arr[0] === self::$thread_id) {
        $data[] = $arr;
      }
      self::$memcpy_signal_file_pos += (strlen($line));
    }

    // calculate & record stats
    if (sort($data) === false) {
      echo "failed to sort data!\n";
      exit;
    }

    $copy = 0.0;
    foreach ($data as $entry) {
      // $timestamp = intval($entry[1]);
      $size = floatval($entry[2]) / (1024 * 1024);

      $copy += $size;
    }
    self::$total_copy += $copy;

    // save samples
    $entry = $trace[1]["file"] . ":" . strval($trace[1]["line"]);
    if (array_key_exists($entry, self::$memcpy_samples)) {
      self::$memcpy_samples[$entry] += $copy;
    } else {
      self::$memcpy_samples[$entry] = $copy;
    }
  }

  public static function open_signal_files()
  {
    // if we are in a thread, $cpu_only is always false
    // in that case, if LD_PRELOAD is present, then we are in full profiling mode
    // otherwise, we are in CPU-only mode, and the second conidtion checks for that
    if (self::$cpu_only || !array_key_exists("LD_PRELOAD", $_ENV)) {
      return;
    }

    $file_name = "/tmp/scalene-malloc-signal" . strval(posix_getpid());
    $handle = fopen($file_name, "r");
    if ($handle === false) {
      echo "fopen() failed for alloc signal file!\n";
      exit;
    } else {
      self::$alloc_signal_file = $handle;
    }

    $file_name = "/tmp/scalene-memcpy-signal" . strval(posix_getpid());
    $handle = fopen($file_name, "r");
    if ($handle === false) {
      echo "fopen() failed for memcpy signal file!\n";
      exit;
    } else {
      self::$memcpy_signal_file = $handle;
    }
  }

  public static function start()
  {
    // update thread id
    self::$thread_id = strval(pcntl_get_thread_id());

    // open signal files
    self::open_signal_files();

    // https://wiki.php.net/rfc/async_signals
    pcntl_async_signals(true);

    // install signal handlers
    if (!pcntl_signal(SIGALRM, "SCALENE\Scalene::signal_dispatch")) {
      echo "pcntl_signal(SIGALRM) failed!\n";
      exit;
    }
    if (!pcntl_signal(SIGVTALRM, "SCALENE\Scalene::signal_dispatch")) {
      echo "pcntl_signal(SIGVTALRM) failed!\n";
      exit;
    }
    if (!pcntl_signal(SIGXCPU, "SCALENE\Scalene::signal_dispatch")) {
      echo "pcntl_signal(SIGXCPU) failed!\n";
      exit;
    }
    if (!pcntl_signal(SIGXFSZ, "SCALENE\Scalene::signal_dispatch")) {
      echo "pcntl_signal(SIGFXSZ) failed!\n";
      exit;
    }
    if (!pcntl_signal(SIGPROF, "SCALENE\Scalene::signal_dispatch")) {
      echo "pcntl_signal(SIGPROF) failed!\n";
      exit;
    }

    // set up timer signal
    if (pcntl_setitimer(ITIMER_VIRTUAL,
      self::$cpu_sampling_rate, self::$cpu_sampling_rate) === -1)
    {
      echo "pcntl_setitimer() failed!\n";
      exit;
    }
    self::update_timestamps();
  }

  public static function end()
  {
    // https://wiki.php.net/rfc/async_signals
    pcntl_async_signals(false);

    // disable signal handlers
    if (!pcntl_signal(SIGALRM, SIG_IGN)) {
      echo "pcntl_signal(SIGALRM) failed!\n";
      exit;
    }
    if (!pcntl_signal(SIGVTALRM, SIG_IGN)) {
      echo "pcntl_signal(SIGVTALRM) failed!\n";
      exit;
    }
    if (!pcntl_signal(SIGXCPU, SIG_IGN)) {
      echo "pcntl_signal(SIGXCPU) failed!\n";
      exit;
    }
    if (!pcntl_signal(SIGXFSZ, SIG_IGN)) {
      echo "pcntl_signal(SIGFXSZ) failed!\n";
      exit;
    }
    if (!pcntl_signal(SIGPROF, SIG_IGN)) {
      echo "pcntl_signal(SIGPROF) failed!\n";
      exit;
    }

    // dump profile
    self::dump_profile();

    // reset
    self::$cpu_samples_php = array();
    self::$cpu_samples_c = array();
    self::$malloc_samples = array();
    self::$malloc_samples_php = array();
    self::$malloc_counters = array();
    self::$free_samples = array();
    self::$memcpy_samples = array();
    self::$current_footprint = 0.0;
    self::$max_footprint = 0.0;
    self::$total_allocs = 0.0;
    self::$total_free = 0.0;
    self::$total_copy = 0.0;
  }

  public static function dump_profile()
  {
    // open stats file
    $file_name = "/tmp/scalene-stats-" . strval(posix_getpid());
    $stats_file = fopen($file_name, "a");
    if ($stats_file === false) {
      echo "fopen() failed for stats file!\n";
      exit;
    }

    // build output
    $output = "";

    foreach (array_keys(self::$cpu_samples_php) as $key) {
      $php_t = round(self::$cpu_samples_php[$key], 2);
      $c_t = round(self::$cpu_samples_c[$key], 2);
      $output .= "c,$key,$php_t,$c_t\n";
      // format: type,file:line,php_time,c_time
    }

    foreach (array_keys(self::$malloc_samples) as $key) {
      $allocs = round(self::$malloc_samples[$key], 2);
      $php_allocs = round(self::$malloc_samples_php[$key], 2);
      $output .= "a,$key,$allocs,$php_allocs\n";
      // format: type,file:line,total_alloc,php_alloc
    }

    foreach (array_keys(self::$free_samples) as $key) {
      $freed = round(self::$free_samples[$key], 2);
      $output .= "f,$key,$freed\n";
      // format: type,file:line,freed
    }

    foreach (array_keys(self::$memcpy_samples) as $key) {
      $copied = round(self::$memcpy_samples[$key], 2);
      $output .= "m,$key,$copied\n";
      // format: type,file:line,copied
    }

    // summary
    // format: type,pid,thread_id,current_footprint,max_footprint,total_alloc,total_free,total_copy
    $output .= "s,";
    $output .= posix_getpid() . ",";
    $output .= self::$thread_id . ",";
    $output .= round(self::$current_footprint, 2) . ",";
    $output .= round(self::$max_footprint, 2) . ",";
    $output .= round(self::$total_allocs, 2) . ",";
    $output .= round(self::$total_free, 2) . ",";
    $output .= round(self::$total_copy, 2) . PHP_EOL;

    // write output
    if (fwrite($stats_file, $output) === false) {
      echo "fwrite() failed for stats file!\n";
      exit;
    }

    // close stats file
    if (!fclose($stats_file)) {
      echo "fclose() failed for stats file!\n";
      exit;
    }
  }

  public static function print_stats()
  {
    // open stats file
    $file_name = "/tmp/scalene-stats-" . strval(posix_getpid());
    $stats_file = fopen($file_name, "r");
    if ($stats_file === false) {
      echo "fopen() failed for stats file!\n";
      exit;
    }

    // read & merge stats (see dump_profile() for formats)
    $cpu_stats = array();
    $alloc_stats = array();
    $free_stats = array();
    $copy_stats = array();
    $summaries = array();

    while (($line = fgets($stats_file)) !== false)
    {
      $arr = explode(",", $line);

      if ($arr[0] === "c") // cpu
      {
        $loc = explode(":", $arr[1]);
        $file = $loc[0];
        $line = intval($loc[1]);

        if (!array_key_exists($file, $cpu_stats)) {
          $cpu_stats[$file] = array();
        }
        $file_arr = &$cpu_stats[$file];

        if (!array_key_exists($line, $file_arr)) {
          $file_arr[$line] = array(0.0, 0.0);
        }
        $line_arr = &$file_arr[$line];

        $line_arr[0] += floatval($arr[2]);
        $line_arr[1] += floatval($arr[3]);
      }
      elseif ($arr[0] === "a") // alloc
      {
        $loc = explode(":", $arr[1]);
        $file = $loc[0];
        $line = intval($loc[1]);

        if (!array_key_exists($file, $alloc_stats)) {
          $alloc_stats[$file] = array();
        }
        $file_arr = &$alloc_stats[$file];

        if (!array_key_exists($line, $file_arr)) {
          $file_arr[$line] = array(0.0, 0.0);
        }
        $line_arr = &$file_arr[$line];

        $line_arr[0] += floatval($arr[2]);
        $line_arr[1] += floatval($arr[3]);
      }
      elseif ($arr[0] === "f") // free
      {
        $loc = explode(":", $arr[1]);
        $file = $loc[0];
        $line = intval($loc[1]);

        if (!array_key_exists($file, $free_stats)) {
          $free_stats[$file] = array();
        }
        $file_arr = &$free_stats[$file];

        if (!array_key_exists($line, $file_arr)) {
          $file_arr[$line] = 0.0;
        }

        $file_arr[$line] += floatval($arr[2]);
      }
      elseif ($arr[0] === "m") // copy
      {
        $loc = explode(":", $arr[1]);
        $file = $loc[0];
        $line = intval($loc[1]);

        if (!array_key_exists($file, $copy_stats)) {
          $copy_stats[$file] = array();
        }
        $file_arr = &$copy_stats[$file];

        if (!array_key_exists($line, $file_arr)) {
          $file_arr[$line] = 0.0;
        }

        $file_arr[$line] += floatval($arr[2]);
      }
      elseif ($arr[0] === "s") // summary
      {
        $pid = intval($arr[1]);
        $thread_id = intval($arr[2]);

        if (!array_key_exists($pid, $summaries)) {
          $summaries[$pid] = array(array(), array(), 0.0, 0.0, 0.0);
        }
        $proc_arr = &$summaries[$pid];

        $proc_arr[0][$thread_id] = floatval($arr[3]);
        $proc_arr[1][$thread_id] = max($proc_arr[1][$thread_id] ?? 0.0, floatval($arr[4]));
        $proc_arr[2] += floatval($arr[5]);
        $proc_arr[3] += floatval($arr[6]);
        $proc_arr[4] += floatval($arr[7]);
      }
      else
      {
        echo "unknown stats type {$arr[0]}!\n";
        exit;
      }
    }

    // print stats
    if (!empty($cpu_stats))
    {
      if (ksort($cpu_stats) === false) {
        echo "failed to sort cpu_stats!\n";
        exit;
      }

      echo "\n==========CPU samples==========\n";
      echo "FILE (LINE): PHP Time (sec) | C Time (sec)\n";

      foreach ($cpu_stats as $file => &$file_arr) {
        if (ksort($file_arr) === false) {
          echo "failed to sort file_arr for $file!\n";
          exit;
        }

        foreach ($file_arr as $line => &$line_arr) {
          $php_t = $line_arr[0];
          $c_t = $line_arr[1];
          echo "$file ($line): $php_t | $c_t\n";
        }
      }
    }

    if (!empty($alloc_stats))
    {
      if (ksort($alloc_stats) === false) {
        echo "failed to sort alloc_stats!\n";
        exit;
      }

      echo "\n==========malloc samples==========\n";
      echo "FILE (LINE): Total Allocated (MiB) | PHP Allocated (MiB)\n";

      foreach ($alloc_stats as $file => &$file_arr) {
        if (ksort($file_arr) === false) {
          echo "failed to sort file_arr for $file!\n";
          exit;
        }

        foreach ($file_arr as $line => &$line_arr) {
          $allocs = $line_arr[0];
          $php_allocs = $line_arr[1];
          echo "$file ($line): $allocs | $php_allocs\n";
        }
      }
    }

    if (!empty($free_stats))
    {
      if (ksort($free_stats) === false) {
        echo "failed to sort free_stats!\n";
        exit;
      }

      echo "\n==========free samples==========\n";
      echo "FILE (LINE): Total Freed (MiB)\n";

      foreach ($free_stats as $file => &$file_arr) {
        if (ksort($file_arr) === false) {
          echo "failed to sort file_arr for $file!\n";
          exit;
        }

        foreach ($file_arr as $line => $freed) {
          echo "$file ($line): $freed\n";
        }
      }
    }

    if (!empty($copy_stats))
    {
      if (ksort($copy_stats) === false) {
        echo "failed to sort copy_stats!\n";
        exit;
      }

      echo "\n==========copy samples==========\n";
      echo "FILE (LINE): Total Copied (MiB)\n";

      foreach ($copy_stats as $file => &$file_arr) {
        if (ksort($file_arr) === false) {
          echo "failed to sort file_arr for $file!\n";
          exit;
        }

        foreach ($file_arr as $line => $copied) {
          echo "$file ($line): $copied\n";
        }
      }
    }

    if (!empty($summaries))
    {
      if (ksort($summaries) === false) {
        echo "failed to sort summaries!\n";
        exit;
      }

      echo "\n==========summaries (per process)==========\n";
      echo "PID: Thread Footprints at Exit (MiB) | Thread Max Footprints (MiB) | Total Allocated (MiB) | Total Freed (MiB) | Total Copied (MiB)\n";

      foreach ($summaries as $pid => &$proc_arr)
      {
        if (ksort($proc_arr[0]) === false) {
          echo "failed to sort current footprints!\n";
          exit;
        }
        if (ksort($proc_arr[1]) === false) {
          echo "failed to sort max footprints!\n";
          exit;
        }

        $current_footprint = json_encode(array_values($proc_arr[0]));
        $max_footprint = json_encode(array_values($proc_arr[1]));
        $total_allocs = $proc_arr[2];
        $total_free = $proc_arr[3];
        $total_copy = $proc_arr[4];
        echo "$pid: $current_footprint | $max_footprint | $total_allocs | $total_free | $total_copy\n";
      }
    }

    echo "========================================\n\n";

    // close & delete stats file
    if (!fclose($stats_file)) {
      echo "fclose() failed for stats file!\n";
      exit;
    }
    if (!unlink($file_name)) {
      echo "unlink() failed for stats file!\n";
      exit;
    }
  }

  private static function should_trace(array $trace): bool
  {
    // don't profile the profiler
    if (strpos($trace[1]["file"], "scalene.php") === false) {
      return true;
    } else {
      return false;
    }
  }

  private static function get_process_time(): float
  {
    $t = 0.0;
    if (pcntl_process_time($t) === -1) {
      echo "pcntl_process_time() failed!\n";
      exit;
    }
    return $t;
  }

  public static function update_timestamps()
  {
    self::$last_signal_time_virt = self::get_process_time();
  }
}

?>
