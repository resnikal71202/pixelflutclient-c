# Pixelfut Client

build with
``` gcc -Wall -O2 -pthread client.c -o client ```

or build the shuffle variant (sends pixels in random order for better visual spread) with
``` gcc -Wall -O2 -pthread shuffle.c -o shuffle ```

run with
``` ./client <image_path> <x_offset> <y_offset> <hostname> <port> <num_threads> ```
