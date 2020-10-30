<?php

include_once("profiler.php");


/* Single Thread */
SCALENE\Scalene::start();
$start = microtime(true);

for ($its = 0; $its < 3; $its++) {
  echo "Elapsed: ", microtime(true) - $start, PHP_EOL;

  for($i = 0; $i < 1000000; $i++) {
    $x = array();
    $x[] = $i;
  }

  for($j = 0; $j < 1000000; $j++) {
    $x = array();
    $x[] = $j;
  }
}
SCALENE\Scalene::end();


/* Multithreading */
$r1 = new \parallel\Runtime();
$r2 = new \parallel\Runtime();

$f1 = $r1->run(function() {
  include_once("profiler.php");
  SCALENE\Scalene::start();
  for($i = 0; $i < 1000000; $i++) {
    $x = array();
    $x[] = $i;
  }
  SCALENE\Scalene::end();
  return true;
});

$f2 = $r2->run(function() {
  include_once("profiler.php");
  SCALENE\Scalene::start();
  for($i = 0; $i < 5000000; $i++) {
    $a = mt_rand() % 100000 + 1;
    $b = mt_rand() % 100 + 1;
    $c = log($a, $b);
  }
  SCALENE\Scalene::end();
  return true;
});

assert($f1->value() && $f2->value());
$r1->close();
$r2->close();


/* Multiprocessing */
$child_proc = array();

$pid = pcntl_fork();
if ($pid === -1)
{
  echo "pcntl_fork() failed!\n";
  exit;
}
elseif ($pid === 0) // child
{
  SCALENE\Scalene::start();
  for($i = 0; $i < 5000000; $i++) {
    $a = mt_rand() % 100000 + 1;
    $b = mt_rand() % 100 + 1;
    $c = log($a, $b);
  }
  SCALENE\Scalene::end();
  exit;
}
else // parent
{
  $child_proc[] = $pid;
}

$pid = pcntl_fork();
if ($pid === -1)
{
  echo "pcntl_fork() failed!\n";
  exit;
}
elseif ($pid === 0) // child
{
  SCALENE\Scalene::start();
  for($j = 0; $j < 1000000; $j++) {
    $x = array();
    $x[] = $j;
  }
  SCALENE\Scalene::end();
  exit;
}
else // parent
{
  $child_proc[] = $pid;
}

foreach ($child_proc as $p) {
  if (pcntl_waitpid($p, $status) == -1) {
    echo "pcntl_waitpid() failed for $p!\n";
    exit;
  } elseif (pcntl_wifexited($status)) {
    echo "$p exited with " . pcntl_wexitstatus($status) . PHP_EOL;
  } elseif (pcntl_wifsignaled($status)) {
    echo "$p terminated by signal " . pcntl_wtermsig($status) . PHP_EOL;
  }
}

?>
