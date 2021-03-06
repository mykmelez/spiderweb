# TODO remove VOLUME below when the base image is updated next.
FROM          taskcluster/centos6-build-upd:0.1.6.20160329195300
MAINTAINER    Dustin J. Mitchell <dustin@mozilla.com>

# TODO remove when base image is updated
VOLUME /home/worker/workspace
VOLUME /home/worker/tooltool-cache
VOLUME /home/worker/hg-shared

# Add build scripts; these are the entry points from the taskcluster worker, and
# operate on environment variables
ADD             bin /home/worker/bin
RUN             chmod +x /home/worker/bin/*

# %include testing/docker/recipes/tooltool.py
ADD topsrcdir/testing/docker/recipes/tooltool.py /builds/tooltool.py
ADD topsrcdir/testing/docker/recipes/tooltool.py /setup/tooltool.py

# %include testing/mozharness/external_tools/robustcheckout.py
ADD topsrcdir/testing/mozharness/external_tools/robustcheckout.py /usr/local/mercurial/robustcheckout.py

# %include testing/docker/recipes/common.sh
ADD topsrcdir/testing/docker/recipes/common.sh /setup/common.sh

# %include testing/docker/recipes/install-mercurial.sh
ADD topsrcdir/testing/docker/recipes/install-mercurial.sh /setup/install-mercurial.sh

# %include testing/docker/recipes/centos6-build-system-setup.sh
ADD topsrcdir/testing/docker/recipes/centos6-build-system-setup.sh /setup/system-setup.sh

# TODO remove once base image doesn't install Mercurial
RUN pip uninstall -y Mercurial

RUN bash /setup/system-setup.sh

# Add wrapper scripts for xvfb allowing tasks to easily retry starting up xvfb
# %include testing/docker/recipes/xvfb.sh
ADD topsrcdir/testing/docker/recipes/xvfb.sh /home/worker/scripts/xvfb.sh

# %include testing/docker/recipes/run-task
ADD topsrcdir/testing/docker/recipes/run-task /home/worker/bin/run-task

# Add configuration
COPY            dot-config                    /home/worker/.config

# Generate machine uuid file
RUN dbus-uuidgen --ensure=/var/lib/dbus/machine-id

# Stubbed out credentials; mozharness looks for this file an issues a WARNING
# if it's not found, which causes the build to fail.  Note that this needs to
# be in the parent of the workspace directory and in the directory where
# mozharness is run (not its --work-dir).  See Bug 1169652.
ADD           oauth.txt /home/worker/

# stubbed out buildprops, which keeps mozharness from choking
# Note that this needs to be in the parent of the workspace directory and in
# the directory where mozharness is run (not its --work-dir)
ADD           buildprops.json /home/worker/

# Move installation to base centos6-build image once Bug 1272629 is fixed
# Install the screen package here to use with xvfb.
# Install bison to build binutils.
RUN yum install -y bison screen

# Set a default command useful for debugging
CMD ["/bin/bash", "--login"]
