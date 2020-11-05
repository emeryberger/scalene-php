<?php
# The Computer Language Benchmarks Game
# https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
#
# contributed by Yuriy Moskalchuk

SCALENE\Scalene::start();

function work()
{
    global $seq, $workers;
    $sockets = stream_socket_pair(1, 1, 0);
    $pid = pcntl_fork();
    if ($pid === 0) {
        SCALENE\Scalene::start();
        fclose($sockets[0]);
        fwrite($sockets[1],
            wordwrap(
                strrev(
                    strtr(
                        str_ireplace("\n", '', $seq),
                        'CGATMKRYVBHDcgatmkryvbhd',
                        'GCTAKMYRBVDHGCTAKMYRBVDH'
                    )
                ),
                60, "\n", true
            )
        );
        SCALENE\Scalene::end();
        exit;
    } else {
        fclose($sockets[1]);
        $workers[] = $sockets[0];
    }
}

$headers = $workers = [];
$seq = '';
while (true) {
    $line = fgets(STDIN);
    if ($line[0] === '>') {
        $headers[] = $line;
        if (!empty($seq)) {
            work();
            $seq = '';
        }
    } else {
        $seq .= $line;
        if (!$line) {
            break;
        }
    }
}
work();

foreach ($workers as $n => $socket) {
    fwrite(STDOUT, $headers[$n]);
    stream_copy_to_stream($socket, STDOUT);
    fwrite(STDOUT, "\n");
}

SCALENE\Scalene::end();
