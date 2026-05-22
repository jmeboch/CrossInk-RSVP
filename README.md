# CrossInk-RSVP

> **CrossInk-RSVP is a personal fork of [CrossInk](https://github.com/uxjulia/CrossInk), itself based on [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).** This fork replaces the standard EPUB/TXT reading flow with an RSVP reader while keeping the CrossInk typography, stats, and quality-of-life improvements.

## What's different in this fork

My goal with this fork is to maintain the core CrossPoint/CrossInk firmware while turning the Xteink reader into an RSVP device: books are shown one word, or a small group of words, at a time. The existing CrossInk typography, lightweight reading statistics, and UI refinements are still included, but the primary reading experience is now built around rapid serial visual presentation.

<table>
  <tr>
    <td align="center">
      <img src="./docs/images/bitter-small-15-margin.jpg" alt="Font: Bitter, Size: Small, Margin: 15" /><br/>
      <em>Font: Bitter, Size: Small, Margin: 15</em>
    </td>
    <td align="center">
      <img src="./docs/images/reading-stats.jpg" alt="Reading Stats with custom front button mapping shown" /><br/>
      <em>Reading Stats with custom front button mapping shown</em>
    </td>
  </tr>
</table>

---

**Note**: This firmware is confirmed to be working on both the X4 but should work on the X3. I do not have one to test with though.

### Highlights

- RSVP reader mode for EPUB and TXT files, with play/pause, back-a-paragraph navigation, and 1-3 words-at-a-time display options
