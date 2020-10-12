<?php

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

// $runtime = new \parallel\Runtime();

// $f1 = $runtime->run(function() {
//   for($i = 0; $i < 1000000; $i++) {
//     $x = array();
//     $x[] = $i;
//   }
//   return true;
// });

// $f2 = $runtime->run(function() {
//   for($i = 0; $i < 5000000; $i++) {
//     $a = mt_rand() % 100000 + 1;
//     $b = mt_rand() % 100 + 1;
//     $c = log($a, $b);
//   }
//   return true;
// });

// assert($f1->value() && $f2->value());

?>
