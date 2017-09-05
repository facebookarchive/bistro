# Test-driving the Bistro UI


## Security: words to the wise

**IMPORTANT:** The "test-drive" Docker-based scripts provided for the UI and
for the back-end are emphatically **NOT** appropriate for production use.

I am paranoid, but If I were you, I would run them in a VM with highly
restricted access to the Internet:
 
 - No inbound connections,
 - Allow outgoing connections only to Github, to the Docker repo hosing the
   Ubuntu image, and to the Ubuntu package-management servers.

Upon completing the setup below, you would fire up a browser inside the VM
to play with the UI.

Some specific security issues with these demos include:

 - Everything runs as `root` -- don't kid yourself, Linux kernel isolation
   used by Docker is not perfect, so this is `root` on the host VM too.
 - Docker's security model -- more in `build/fbcode_builder/README.docker`.
 - Lack of code hash / signature verification on the Github code.
 - Lack of production hardening on the Phabricator set-up.
 - Lack of production hardening on the Bistro back-end setup.
 - Default MySQL password in containers.
 - `bistro.get-state-hostname-suffix` is set to '', instead of a safe value.

The above list is definitely not complete.


## Start the backend first

First, you'll want a running back-end. The quickest way to get one is:
 - Use `build/fbcode_builder/README.md` to quickly build the binaries,
 - Follow `bistro/scripts/docker_toy/README.md` to start a toy deployment.

Once that's ready, let's bring up a second Docker image with a web server
hosting the Bistro UI on port 80.


## Docker build

Run this

  ./docker_build.sh &> docker_build.log ; tail docker_build.log


## Start the Bistro UI in a Docker container

Start an interactive session in the UI image you just built:

  docker run --rm -it --expose 80 $(docker images -q | head -n1) /bin/bash
  service mysql start
  service apache2 start


## Connect to the Bistro UI from outside the container

To connect, your browser will need to send a request to the container's IP
address with `phabdocker.example.com` in the `Host` header.  The easiest way
is to add an appropriate line to `/etc/hosts` via the command below.  If you
already have a browser extension that lets you set the `Host` header for
IPs, use that instead.

  sudo apt-get install jq  # If not already installed.
  (
    echo "# Phabricator docker image virtual host"
    echo "$(docker inspect $(docker ps -q | head -n1) | 
              jq -r .[0].NetworkSettings.IPAddress) phabdocker.example.com"
  ) | sudo tee -a /etc/hosts

*Reminder:* clean up your `/etc/hosts` once you're done test-driving Bistro.

Now, you can go to `http://phabdocker.example.com` in your browser. Phabricator
will ask you to register an admin account. Enter some fake data to continue.

After account setup:
 - Go to `http://phabdocker.example.com/bistro`
 - Click the "Hostport List" radio button next to "Host:port Source"
 - Enter `BACKEND_CONTAINER_IP:31415` into "Host:port Data".
 - Click "View Jobs". 
 - The next page will have colored bars representing groups of tasks. 
   Click on one of the colored areas, then click on a task links in the
   revealed panel to see the task's logs.


## Toy example caveats

- Read `Security: word to the wise` at the top of this file!

- Your Docker-based UI will be in UTC. Change the container's timezone if
  that's annoying.

- The in-container Phabricator install has a lot of set-up issues. This is a
  minimal example, for production use you would want to address them.


## Docker disk usage tip: finding images unused by the latest build

If you are building a lot of Docker containers, they tend to eat up disk
quickly.  This command will list image IDs that are **NOT** in use by your
latest build:

  sort <(docker images -qa) <(
    grep '^ ---> [0-9a-f]\+$' docker_build.log | cut -f 3- -d\  | sed p
  ) | uniq -u

Upon checking the output, you can wrap the command with `docker rmi $(...)`
to delete the images.
