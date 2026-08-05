/**
 * GitHub alert blockquotes, rendered as Starlight asides.
 *
 *   > [!WARNING]
 *   > **Watch for shorts**
 *   >
 *   > The BeagleBone is a little fat, and some of its parts can touch the
 *   > solder side of a module sitting above it.
 *
 * GitHub renders the alert natively; Starlight understands `:::caution` and
 * nothing else. This turns the one into the other, so a page carries a callout
 * that survives being read in git.
 *
 * A bolded paragraph at the head of the body becomes the aside's title. Starlight
 * takes a title from a directive label, which the alert syntax has no room for.
 */
import { visit } from 'unist-util-visit';

/** GitHub's alert vocabulary, mapped onto Starlight's four aside variants. */
const VARIANTS = new Map([
	['NOTE', 'note'],
	['TIP', 'tip'],
	['IMPORTANT', 'note'],
	['WARNING', 'caution'],
	['CAUTION', 'danger'],
]);

const ALERT = /^\[!([A-Z]+)\]\s*$/;

export function remarkGithubAlerts() {
	return () => (tree) => {
		visit(tree, 'blockquote', (node, index, parent) => {
			if (!parent || index === undefined) return;

			const [first, ...body] = node.children;
			if (first?.type !== 'paragraph') return;

			// The marker sits alone on the blockquote's first line; the rest of that
			// paragraph, if any, is the start of the body.
			const [marker] = first.children;
			if (marker?.type !== 'text') return;

			const [line, ...rest] = marker.value.split('\n');
			const variant = VARIANTS.get(ALERT.exec(line)?.[1] ?? '');
			if (!variant) return;

			const remainder = rest.join('\n');
			const opening = [
				...(remainder ? [{ type: 'text', value: remainder }] : []),
				...first.children.slice(1),
			];
			const children = opening.length
				? [{ type: 'paragraph', children: opening }, ...body]
				: body;

			// A bolded lead paragraph names the aside; Starlight reads that from a
			// paragraph flagged as the directive's label.
			const [lead] = children;
			if (lead?.type === 'paragraph' && lead.children.length === 1 && lead.children[0]?.type === 'strong') {
				children[0] = {
					type: 'paragraph',
					data: { directiveLabel: true },
					children: lead.children[0].children,
				};
			}

			parent.children[index] = { type: 'containerDirective', name: variant, children };
		});
	};
}
