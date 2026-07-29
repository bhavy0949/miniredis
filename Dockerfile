# miniredis runs on Linux (it uses epoll). This image lets you build & run it
# anywhere Docker is available — including macOS/Windows.
#
#   docker build -t miniredis .
#   docker run --rm -p 6379:6379 miniredis
#
# then from another terminal:  nc 127.0.0.1 6379
FROM gcc:14
WORKDIR /app
COPY . .
RUN make
EXPOSE 6379
CMD ["./miniredis"]
