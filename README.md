# FakeVive

[Google Earth VR](http://store.steampowered.com/app/348250/) was released recently,
but it includes a hardware check that throws an error if you aren't using a Vive.

FakeVive is a DLL which you can drop into the app's folder to make it believe that a Vive is connected.
It's not a reverse Revive or anything, it just intercepts the OpenVR requests for HMD information and spoofs the model string.
I don't care to make it much more complex than this because SteamVR already supports the Rift fairly well.

FakeVive doesn't involve modifying any of the app's files and it doesn't do anything specific to Google Earth VR.
This makes it easy to install and it will continue to work even if the app is updated (assuming the HMD check isn't made more
complex). It could probably even be made to work if future apps try to do a similar check.

## How to Install

1. Grab the latest version from the [Releases page](https://github.com/Shockfire/FakeVive/releases).

2. Find where the Vive-locked app is installed. For Google Earth VR, you can do this easily by right-clicking the app on Steam,
clicking "Properties", going to the "Local files" tab, and then clicking "Browse local files".

3. Unzip FakeVive into the app's folder. You should now at least see ddraw.dll and the app EXE in the same folder.

4. Run the app through Steam or however you prefer.
