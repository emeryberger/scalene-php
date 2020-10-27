<?php

include_once("profiler.php");


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

?>
