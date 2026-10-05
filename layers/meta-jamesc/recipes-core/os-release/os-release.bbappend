# Expose the build identifier in /etc/os-release so the REST API can report
# which firmware artifact is running. Yocto formats the default as UTC
# YYYYMMDDhhmmss.
OS_RELEASE_FIELDS:append = " BUILD_ID"
